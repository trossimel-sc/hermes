/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/AST/ES6Class.h"
#include "hermes/AST/RecursiveVisitor.h"
#include "hermes/Parser/JSLexer.h"
#include "llvh/ADT/StringRef.h"

namespace {
using namespace hermes;

/// Mutable vector that helps dealing with arrays of nodes safely.
/// Once done with the vector, it can create an ESTree::NodeList
/// representation which is used by the ESTree API in several places.
class NodeVector {
 public:
  using Storage = llvh::SmallVector<ESTree::Node *, 8>;

  NodeVector() = default;
  NodeVector(std::initializer_list<ESTree::Node *> nodes) {
    for (auto &node : nodes) {
      _storage.push_back(node);
    }
  }

  NodeVector(ESTree::NodeList &list) {
    for (auto &node : list) {
      _storage.push_back(&node);
    }
  }

  ~NodeVector() = default;

  size_t size() const {
    return _storage.size();
  }

  Storage::const_iterator begin() const {
    return _storage.begin();
  }

  Storage::const_iterator end() const {
    return _storage.end();
  }

  void append(ESTree::Node *node) {
    _storage.emplace_back(node);
  }

  void prepend(ESTree::Node *node) {
    _storage.insert(_storage.begin(), node);
  }

  ESTree::NodeList toNodeList() const {
    ESTree::NodeList nodeList;
    for (auto &node : _storage) {
      nodeList.push_back(*node);
    }

    return nodeList;
  }

 private:
  Storage _storage;
};

struct VisitedClass {
  UniqueString *className = nullptr;
  ESTree::Node *parentClass = nullptr;
  bool superCallFound = false;

  VisitedClass(ESTree::NodePtr className, ESTree::NodePtr superClass) {
    if (className != nullptr) {
      this->className = llvh::cast<ESTree::IdentifierNode>(className)->_name;
    }
    this->parentClass = superClass;
  }
};

enum class ClassMemberKind {
  Constructor,
  Method,
  PropertyGetter,
  PropertySetter
};

struct ResolvedClassMember {
  ESTree::Node *key;
  bool isStatic;
  ClassMemberKind kind;
  ESTree::MethodDefinitionNode *definitionNode;

  ResolvedClassMember(
      ESTree::Node *key,
      bool isStatic,
      ClassMemberKind kind,
      ESTree::MethodDefinitionNode *definitionNode)
      : key(key),
        isStatic(isStatic),
        kind(kind),
        definitionNode(definitionNode) {}
};

struct ResolvedClassMembers {
  ESTree::MethodDefinitionNode *constructor = nullptr;

  llvh::SmallVector<ResolvedClassMember, 8> members;
};

static ClassMemberKind getClassMemberKind(
    ESTree::MethodDefinitionNode *methodDefinition) {
  const auto &str = methodDefinition->_kind->str();
  if (str == "constructor") {
    return ClassMemberKind::Constructor;
  } else if (str == "method") {
    return ClassMemberKind::Method;
  } else if (str == "get") {
    return ClassMemberKind::PropertyGetter;
  } else if (str == "set") {
    return ClassMemberKind::PropertySetter;
  }
  hermes_fatal("Invalid ES6 class member");
}

} // namespace

namespace hermes {

/// Visitor that visits Class declarations and Class expressions and convert
/// them into plain ES5 functions. The generated AST leverages the
/// HermesES6Internal object, which should be made available at runtime by
/// enabling the ES6Class option.
class ES6ClassesTransformations
    : public ESTree::RecursionDepthTracker<ES6ClassesTransformations> {
 public:
  /// Required by ESTree::RecursiveVisitorDispatch.
  static constexpr bool kEnableNodeListMutation = true;

  ES6ClassesTransformations(Context &context)
      : context_(context),
        identVar_(context.getIdentifier("let").getUnderlyingPointer()),
        internalThis(context.getIdentifier("__hermes_internal_this__")
            .getUnderlyingPointer()) {}

  // when true, recursively replace ThisExpressionNode with
  // __hermes_internal_this__
  bool replaceThis = false;

  void visit(ESTree::ClassDeclarationNode *classDecl, ESTree::Node **ppNode) {
    auto oldReplaceThis = replaceThis;
    replaceThis = false;

    auto *classBody = llvh::dyn_cast<ESTree::ClassBodyNode>(classDecl->_body);
    if (classBody == nullptr) {
      return doVisitChildren(classDecl);
    }

    auto *expressionResult = createClass(
        classDecl, classDecl->_id, classBody, classDecl->_superClass);

    auto result = makeSingleLetDecl(
        expressionResult, copyIdentifier(classDecl->_id), expressionResult);

    *ppNode = result;
    replaceThis = oldReplaceThis;
  }

  void visit(ESTree::ClassExpressionNode *classExpr, ESTree::Node **ppNode) {
    bool oldReplaceThis = replaceThis;
    replaceThis = false;

    auto *classBody = llvh::dyn_cast<ESTree::ClassBodyNode>(classExpr->_body);
    if (classBody == nullptr) {
      return doVisitChildren(classExpr);
    }

    *ppNode = createClass(
        classExpr, classExpr->_id, classBody, classExpr->_superClass);
    replaceThis = oldReplaceThis;
  }

  /// Visits call expressions nodes to convert super ctor invocations like
  /// `super(params...)`, or super method invocations like
  /// `super.method(params...)`
  void visit(
      ESTree::CallExpressionNode *callExpression,
      ESTree::Node **ppNode) {
    auto *topClass = _currentProcessingClass;
    if (topClass == nullptr || topClass->parentClass == nullptr) {
      return doVisitChildren(callExpression);
    }

    if (callExpression->_callee->getKind() == ESTree::NodeKind::Super) {
      // Convert super(...args) calls
      _currentProcessingClass->superCallFound = true;
      *ppNode = createSuperCall(
          callExpression,
          makeIdentifierNode(callExpression, topClass->className),
          topClass->parentClass,
          NodeVector(callExpression->_arguments));
      return;
    }

    // Convert super.method(...args) calls to
    // ParentClass.prototype.method.call(this, ...args);
    auto *memberExpressionNode =
        llvh::dyn_cast<ESTree::MemberExpressionNode>(callExpression->_callee);
    if (memberExpressionNode == nullptr ||
        memberExpressionNode->_object->getKind() != ESTree::NodeKind::Super) {
      return doVisitChildren(callExpression);
    }

    *ppNode = createSuperMethodCall(
        callExpression,
        topClass->parentClass,
        memberExpressionNode->_property,
        NodeVector(callExpression->_arguments));
  }

  /// Visits member expression nodes to convert super access, like
  /// `super.property`.
  void visit(
      ESTree::MemberExpressionNode *memberExpression,
      ESTree::Node **ppNode) {
    // Convert super.property into Reflect.get(ParentClass.prototype,
    // 'property', this);
    if (memberExpression->_object->getKind() != ESTree::NodeKind::Super) {
      return doVisitChildren(memberExpression);
    }

    auto *topClass = _currentProcessingClass;
    if (topClass == nullptr || topClass->parentClass == nullptr) {
      // Should not happen
      return doVisitChildren(memberExpression);
    }

    *ppNode = createGetSuperProperty(
        memberExpression, topClass->parentClass, memberExpression->_property);
  }

  void visit(ESTree::ThisExpressionNode *classDecl, ESTree::Node **ppNode) {
    if (replaceThis) {
      *ppNode = makeIdentifierNode(classDecl, internalThis);
    }
  }

  template <typename T>
  typename std::enable_if<
      std::is_same<T, ESTree::FunctionExpressionNode>::value ||
      std::is_same<T, ESTree::FunctionDeclarationNode>::value>::type
  visit(T *node, ESTree::Node **ppNode) {
    // When entering a function, we have a different "this"
    replaceThisCalls(node, false);
  }

  void visit(ESTree::Node *node) {
    visitESTreeChildren(*this, node);
  }

  void recursionDepthExceeded(ESTree::Node *n) {
    context_.getSourceErrorManager().error(
        n->getEndLoc(), "Too many nested expressions/statements/declarations");
  }

 private:
  Context &context_;
  UniqueString *const identVar_;
  UniqueString *const internalThis;
  VisitedClass *_currentProcessingClass = nullptr;
  const ResolvedClassMember *_currentClassMember = nullptr;

  void doVisitChildren(ESTree::Node *node) {
    visitESTreeChildren(*this, node);
  }

  void doCopyLocation(ESTree::Node *src, ESTree::Node *dest) {
    if (src != nullptr) {
      dest->setStartLoc(src->getStartLoc());
      dest->setEndLoc(src->getEndLoc());
      dest->setDebugLoc(src->getDebugLoc());
    }
  }

  template <typename T>
  T *copyLocation(ESTree::Node *src, T *dest) {
    doCopyLocation(src, dest);
    return dest;
  }

  template <typename T, typename... Args>
  T *createTransformedNode(ESTree::Node *src, Args &&...args) {
    auto *node = new (context_) T(std::forward<Args>(args)...);
    return copyLocation(src, node);
  }

  ESTree::Node *cloneNodeInternal(ESTree::Node *node) {
    if (node == nullptr) {
      return nullptr;
    }

    // TODO: Is there a better way to do this?
    if (auto *identifier = llvh::dyn_cast<ESTree::IdentifierNode>(node)) {
      return createTransformedNode<ESTree::IdentifierNode>(
          node,
          identifier->_name,
          cloneNode(identifier->_typeAnnotation),
          identifier->_optional);
    }
    if (auto *memberExpression =
            llvh::dyn_cast<ESTree::MemberExpressionNode>(node)) {
      return createTransformedNode<ESTree::MemberExpressionNode>(
          node,
          cloneNode(memberExpression->_object),
          cloneNode(memberExpression->_property),
          memberExpression->_computed);
    }

    llvm_unreachable("Unsupported Node Kind");

    return node;
  }

  template <typename T>
  T *cloneNode(T *node) {
    return llvh::cast_or_null<T>(cloneNodeInternal(node));
  }

  ESTree::Node *createClass(
      ESTree::Node *classNode,
      ESTree::Node *id,
      ESTree::ClassBodyNode *classBody,
      ESTree::Node *superClass) {
    ESTree::Node *resolvedClassId = nullptr;
    if (id == nullptr) {
      resolvedClassId = makeIdentifierNode(nullptr, "__clsExpr__");
    } else {
      resolvedClassId = id;
    }

    NodeVector statements;

    ESTree::Node *superClassExpr = nullptr;
    ESTree::Node *currentProcessingSuperClass = nullptr;
    if (superClass != nullptr) {
      superClassExpr = makeIdentifierNode(nullptr, "__super__");
      currentProcessingSuperClass = superClassExpr;
      statements.append(makeSingleLetDecl(
          superClass, cloneNode(superClassExpr), cloneNode(superClass)));
      superClassExpr = cloneNode(superClass);
    } else {
      superClassExpr = new (context_) ESTree::NullLiteralNode();
    }

    auto *oldProcessingClass = _currentProcessingClass;
    VisitedClass currentProcessingClass(
        resolvedClassId, currentProcessingSuperClass);
    _currentProcessingClass = &currentProcessingClass;

    auto classMembers = resolveClassMembers(classBody);
    auto *ctorAsFunction = createClassCtor(
        resolvedClassId, classBody, superClass, classMembers.constructor);

    auto *defineClassResult = makeHermesES6InternalCall(
        classNode,
        "defineClass",
        {copyIdentifier(ctorAsFunction->_id), superClassExpr});

    statements.append(ctorAsFunction);
    statements.append(toStatement(defineClassResult));

    appendMethods(resolvedClassId, classMembers, statements);

    // Wrap into an immediately invoked function
    auto *expr = blockToExpression(classNode, statements, ctorAsFunction->_id);

    _currentProcessingClass = oldProcessingClass;

    return expr;
  }

  ESTree::StatementNode *toStatement(ESTree::Node *expression) {
    return createTransformedNode<ESTree::ExpressionStatementNode>(
        expression, expression, nullptr);
  }

  ESTree::IdentifierNode *copyIdentifier(ESTree::Node *identifer) {
    auto *typedIdentifier = llvh::cast<ESTree::IdentifierNode>(identifer);
    return createTransformedNode<ESTree::IdentifierNode>(
        identifer, typedIdentifier->_name, nullptr, false);
  }

  ESTree::Node *makeSingleLetDecl(
      ESTree::Node *srcNode,
      ESTree::Node *identifier,
      ESTree::Node *value) {
    auto *variableDeclarator =
        createTransformedNode<ESTree::VariableDeclaratorNode>(
            srcNode, value, identifier);
    ESTree::NodeList variableList;
    variableList.push_back(*variableDeclarator);
    return createTransformedNode<ESTree::VariableDeclarationNode>(
        srcNode, identVar_, std::move(variableList));
  }

  ESTree::Node *makeHermesES6InternalCall(
      ESTree::Node *srcNode,
      llvh::StringRef methodName,
      const NodeVector &parameters) {
    auto hermesInternalIdentifier =
        makeIdentifierNode(srcNode, "HermesES6Internal");
    auto methodIdentifier = makeIdentifierNode(srcNode, methodName);

    auto *getPropertyNode = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode, hermesInternalIdentifier, methodIdentifier, false);
    return createTransformedNode<ESTree::CallExpressionNode>(
        srcNode, getPropertyNode, nullptr, parameters.toNodeList());
  }

  ESTree::IdentifierNode *makeIdentifierNode(
      ESTree::Node *srcNode,
      UniqueString *name) {
    return createTransformedNode<ESTree::IdentifierNode>(
        srcNode, name, nullptr, false);
  }

  ESTree::IdentifierNode *makeIdentifierNode(
      ESTree::Node *srcNode,
      llvh::StringRef name) {
    return makeIdentifierNode(
        srcNode, context_.getIdentifier(name).getUnderlyingPointer());
  }

  ESTree::Node *makeUndefinedNode(ESTree::Node *srcNode) {
    return makeIdentifierNode(srcNode, "undefined");
  }

  ESTree::Node *createCallWithForwardedThis(
      ESTree::Node *srcNode,
      ESTree::Node *object,
      NodeVector parameters) {
    auto *this_ = createTransformedNode<ESTree::ThisExpressionNode>(srcNode);

    parameters.prepend(this_);

    auto methodIdentifier = makeIdentifierNode(srcNode, "call");

    auto *getPropertyNode = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode, object, methodIdentifier, false);
    return createTransformedNode<ESTree::CallExpressionNode>(
        srcNode, getPropertyNode, nullptr, parameters.toNodeList());
  }

  ESTree::Node *createSuperCall(
      ESTree::Node *srcNode,
      ESTree::Node *baseClass,
      ESTree::Node *superClass,
      NodeVector parameters) {
    // We need to use Reflect.construct (or new) to call the super constructor,
    // as calling super.apply(this, ...args) or super.apply(this, ...args) may
    // not always return the class instance. For example, Date.call(this)
    // returns a string and modifies "this", while Array.call(this) does not
    // modify "this" but returns a new array.

    auto *parametersArray = createTransformedNode<ESTree::ArrayExpressionNode>(
        srcNode, parameters.toNodeList(), false);

    auto *getPropertyNode = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode,
        makeIdentifierNode(srcNode, "Reflect"),
        makeIdentifierNode(srcNode, "construct"),
        false);
    auto *callExpr = createTransformedNode<ESTree::CallExpressionNode>(
        srcNode,
        getPropertyNode,
        nullptr,
        NodeVector{makeIdentifierNode(superClass, "__super__"), parametersArray, cloneNode(baseClass)}.toNodeList());

    auto *assignmentExpression =
        createTransformedNode<ESTree::AssignmentExpressionNode>(
            srcNode,
            context_.getStringTable().getString("="),
            makeIdentifierNode(srcNode, internalThis),
            callExpr);

    return assignmentExpression;
  }

  ESTree::Node *createGetSuperProperty(
      ESTree::Node *srcNode,
      ESTree::Node *superClass,
      ESTree::Node *propertyName) {
    auto *reflectGet = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode,
        makeIdentifierNode(srcNode, "Reflect"),
        makeIdentifierNode(srcNode, "get"),
        false);

    ESTree::NodeList parameters;
    if (_currentClassMember && _currentClassMember->isStatic) {
      // Reflect.get(ParentClass, 'property', this);
      parameters.push_back(*makeIdentifierNode(superClass, "__super__"));
    } else {
      // Reflect.get(ParentClass.prototype, 'property', this);
      auto *getParentClassPrototype =
          createTransformedNode<ESTree::MemberExpressionNode>(
              srcNode,
              makeIdentifierNode(superClass, "__super__"),
              makeIdentifierNode(srcNode, "prototype"),
              false);
      parameters.push_back(*getParentClassPrototype);
    }

    auto *propertyStringLiteral =
        createTransformedNode<ESTree::StringLiteralNode>(
            propertyName,
            llvh::cast<ESTree::IdentifierNode>(propertyName)->_name);
    auto *this_ = createTransformedNode<ESTree::ThisExpressionNode>(srcNode);

    parameters.push_back(*propertyStringLiteral);
    parameters.push_back(*this_);

    return createTransformedNode<ESTree::CallExpressionNode>(
        srcNode, reflectGet, nullptr, std::move(parameters));
  }

  ESTree::Node *createSuperMethodCall(
      ESTree::Node *srcNode,
      ESTree::Node *superClass,
      ESTree::NodePtr property,
      NodeVector parameters) {
    ESTree::Node *getMethodNodeParameter = nullptr;
    if (_currentClassMember && _currentClassMember->isStatic) {
      // Convert super.method(...args) calls to ParentClass.method.call(this,
      // ...args);
      getMethodNodeParameter = makeIdentifierNode(superClass, "__super__");
    } else {
      // Convert super.method(...args) calls to
      // ParentClass.prototype.method.call(this, ...args);
      auto prototypeIdentifier = makeIdentifierNode(srcNode, "prototype");

      getMethodNodeParameter =
          createTransformedNode<ESTree::MemberExpressionNode>(
              srcNode, makeIdentifierNode(superClass, "__super__"), prototypeIdentifier, false);
    }

    auto *getMethodNode = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode, getMethodNodeParameter, property, false);

    return createCallWithForwardedThis(
        srcNode, getMethodNode, std::move(parameters));
  }

  ESTree::Node *blockToExpression(
      ESTree::Node *srcNode,
      const NodeVector &statements,
      ESTree::Node *returnVariableName) {
    auto stmtList = statements.toNodeList();

    auto *returnStmt = createTransformedNode<ESTree::ReturnStatementNode>(
        srcNode, copyIdentifier(returnVariableName));

    stmtList.push_back(*returnStmt);

    auto *body = createTransformedNode<ESTree::BlockStatementNode>(
        srcNode, std::move(stmtList));

    auto *immediateInvokedFunction =
        createTransformedNode<ESTree::FunctionExpressionNode>(
            srcNode,
            nullptr,
            ESTree::NodeList(),
            body,
            nullptr,
            nullptr,
            nullptr,
            false,
            false);

    return createTransformedNode<ESTree::CallExpressionNode>(
        srcNode, immediateInvokedFunction, nullptr, ESTree::NodeList());
  }

  void unpackStatements(ESTree::Node *stmt, NodeVector &out) {
    auto *expressionStatement =
        llvh::dyn_cast<ESTree::ExpressionStatementNode>(stmt);
    if (expressionStatement == nullptr) {
      out.append(stmt);
      return;
    }

    auto *sequenceExpression = llvh::dyn_cast<ESTree::SequenceExpressionNode>(
        expressionStatement->_expression);
    if (sequenceExpression == nullptr) {
      out.append(stmt);
      return;
    }

    NodeVector expressions(sequenceExpression->_expressions);
    for (auto *node : expressions) {
      auto *emittedExpressionStatement =
          createTransformedNode<ESTree::ExpressionStatementNode>(
              node, node, expressionStatement->_directive);

      out.append(emittedExpressionStatement);
    }
  }

  ESTree::FunctionDeclarationNode *createClassCtor(
      ESTree::Node *identifier,
      ESTree::ClassBodyNode *classBody,
      ESTree::Node *superClass,
      ESTree::MethodDefinitionNode *existingCtor) {
    ESTree::NodeList paramList;
    ESTree::NodeList ctorStatements;

    auto isDerived = superClass != nullptr;
    if (isDerived) {
      // var __hermes_internal_this__ = new __super__();
      ESTree::Node *newSuperExpr;
      newSuperExpr = createTransformedNode<ESTree::NewExpressionNode>(
          superClass, makeIdentifierNode(superClass, "__super__"), nullptr, ESTree::NodeList());
      auto *thisVarDecl = makeSingleLetDecl(
          superClass,
          makeIdentifierNode(superClass, internalThis),
          newSuperExpr);
      ctorStatements.push_back(*thisVarDecl);

      // Object.setPrototypeOf(__hermes_internal_this__, MyClass.prototype);
      auto *objectIdentifier = makeIdentifierNode(superClass, "Object");
      auto *setPrototypeOfIdentifier =
          makeIdentifierNode(superClass, "setPrototypeOf");

      auto *setPrototypeOfMethod =
          createTransformedNode<ESTree::MemberExpressionNode>(
              superClass, objectIdentifier, setPrototypeOfIdentifier, false);

      auto *thisExpression = makeIdentifierNode(superClass, internalThis);

      auto *classPrototype =
          createTransformedNode<ESTree::MemberExpressionNode>(
              superClass,
              cloneNode(identifier),
              makeIdentifierNode(superClass, "prototype"),
              false);

      ESTree::NodeList setPrototypeOfArgs;
      setPrototypeOfArgs.push_back(*thisExpression);
      setPrototypeOfArgs.push_back(*classPrototype);

      auto *setPrototypeOfCall =
          createTransformedNode<ESTree::CallExpressionNode>(
              superClass,
              setPrototypeOfMethod,
              nullptr,
              std::move(setPrototypeOfArgs));

      ctorStatements.push_back(*toStatement(setPrototypeOfCall));
    }

    if (existingCtor != nullptr) {
      auto *ctorExpression =
          llvh::dyn_cast<ESTree::FunctionExpressionNode>(existingCtor->_value);
      paramList = std::move(ctorExpression->_params);

      auto *block =
          llvh::dyn_cast<ESTree::BlockStatementNode>(ctorExpression->_body);
      NodeVector tmpStatements;
      for (auto &stmt : block->_body) {
        unpackStatements(&stmt, tmpStatements);
      }
      auto addedPropertyInitializers = false;

      if (!isDerived) {
        // Append property initializers at beginning if no super class
        addedPropertyInitializers = true;
        appendPropertyInitializers(classBody, ctorStatements);
      }

      for (auto &stmt : tmpStatements) {
        auto oldSuperCallFound = _currentProcessingClass->superCallFound;
        visitESTreeChildren(*this, stmt);
        ctorStatements.push_back(*stmt);
        if (!addedPropertyInitializers && !oldSuperCallFound &&
            _currentProcessingClass->superCallFound) {
          // We just processed the super() call.
          // Append initializers of class properties
          addedPropertyInitializers = true;
          appendPropertyInitializers(classBody, ctorStatements);
        }
      }
    } else {
      // No existing ctor.
      if (isDerived) {
        // Generate call to super()
        auto *argumentsSpread =
            createTransformedNode<ESTree::SpreadElementNode>(
                superClass, makeIdentifierNode(superClass, "arguments"));
        auto *superCall = createSuperCall(
            classBody,
            cloneNode(identifier),
            cloneNode(superClass),
            {argumentsSpread});
        ctorStatements.push_back(*toStatement(superCall));
      }

      // Append initializers of class properties
      appendPropertyInitializers(classBody, ctorStatements);
    }

    if (isDerived) {
      auto *returnThisNode = createTransformedNode<ESTree::ReturnStatementNode>(
          classBody, makeIdentifierNode(classBody, internalThis));
      ctorStatements.push_back(*returnThisNode);
    }

    auto *body = createTransformedNode<ESTree::BlockStatementNode>(
        classBody, std::move(ctorStatements));

    replaceThisCalls(body, isDerived);

    return createTransformedNode<ESTree::FunctionDeclarationNode>(
        classBody,
        identifier,
        std::move(paramList),
        body,
        nullptr,
        nullptr,
        nullptr,
        false,
        false);
  }

  void appendPropertyInitializers(
      ESTree::ClassBodyNode *classBody,
      ESTree::NodeList &stmtList) {
    for (auto &entry : classBody->_body) {
      if (auto *classProperty =
              llvh::dyn_cast<ESTree::ClassPropertyNode>(&entry)) {
        if (classProperty->_value != nullptr) {
          visitESTreeNode(*this, classProperty->_value, classProperty);
          auto *initializer = createThisPropertyInitializer(
              classProperty, classProperty->_key, classProperty->_value);
          stmtList.push_back(*initializer);
        }
      }
    }
  }

  ResolvedClassMembers resolveClassMembers(ESTree::ClassBodyNode *classBody) {
    ResolvedClassMembers resolvedClassMembers;

    for (auto &entry : classBody->_body) {
      if (auto *methodDefinition =
              llvh::dyn_cast<ESTree::MethodDefinitionNode>(&entry)) {
        auto memberKind = getClassMemberKind(methodDefinition);

        if (memberKind == ClassMemberKind::Constructor) {
          resolvedClassMembers.constructor = methodDefinition;
          continue;
        }

        resolvedClassMembers.members.emplace_back(
            methodDefinition->_key,
            methodDefinition->_static,
            memberKind,
            methodDefinition);
      }
    }

    return resolvedClassMembers;
  }

  void visitMethodESTreeChildren(
      const ResolvedClassMember &classMember,
      ESTree::Node *node) {
    auto *previousClassMember = _currentClassMember;
    _currentClassMember = &classMember;
    visitESTreeChildren(*this, node);
    _currentClassMember = previousClassMember;
  }

  void appendMethods(
      ESTree::Node *className,
      const ResolvedClassMembers &classMembers,
      NodeVector &stmtList) {
    for (const auto &classMember : classMembers.members) {
      auto *srcNode = classMember.definitionNode;
      visitMethodESTreeChildren(classMember, srcNode);

      NodeVector parameters;
      parameters.append(copyIdentifier(className));

      if (llvh::isa<ESTree::IdentifierNode>(classMember.key)) {
        // Turn identifier into a string literal so that we can pass it
        // as a parameter to the defineClassProperty / defineClassMethod
        // methods.
        auto *identifierNode =
            llvh::cast<ESTree::IdentifierNode>(classMember.key);
        parameters.append(createTransformedNode<ESTree::StringLiteralNode>(
            identifierNode, identifierNode->_name));

        auto *functionExpr =
            llvh::cast<ESTree::FunctionExpressionNode>(srcNode->_value);
        // Prefix and Suffix method name with # to prevent symbol resolution
        // conflicts. The function name will be re-added at runtime

        auto newIdentifierNode = cloneNode(identifierNode);
        newIdentifierNode->_name = context_.getStringTable().getString(
            ("#" + newIdentifierNode->_name->str() + "#").str());

        functionExpr->_id = newIdentifierNode;
        parameters.append(functionExpr);
      } else {
        parameters.append(cloneNode(classMember.key));
        parameters.append(srcNode->_value);
      }

      llvh::StringRef hermesCallName;

      switch (classMember.kind) {
        case ClassMemberKind::Method:
          hermesCallName = classMember.isStatic ? "defineStaticClassMethod"
                                                : "defineClassMethod";
          break;
        case ClassMemberKind::PropertyGetter:
          hermesCallName = classMember.isStatic
              ? "defineStaticClassPropertyGetter"
              : "defineClassPropertyGetter";
          break;
        case ClassMemberKind::PropertySetter:
          hermesCallName = classMember.isStatic
              ? "defineStaticClassPropertySetter"
              : "defineClassPropertySetter";
          break;
        default:
          hermes_fatal("Invalid ES6 class member");
          break;
      }

      auto *call =
          makeHermesES6InternalCall(srcNode, hermesCallName, parameters);

      stmtList.append(toStatement(call));
    }
  }

  ESTree::Node *createThisPropertyInitializer(
      ESTree::Node *srcNode,
      ESTree::Node *identifier,
      ESTree::Node *initialValue) {
    auto *this_ = createTransformedNode<ESTree::ThisExpressionNode>(srcNode);

    auto *getPropertyNode = createTransformedNode<ESTree::MemberExpressionNode>(
        srcNode, this_, identifier, false);
    auto *assignmentExpression =
        createTransformedNode<ESTree::AssignmentExpressionNode>(
            srcNode,
            context_.getStringTable().getString("="),
            getPropertyNode,
            initialValue);

    return toStatement(assignmentExpression);
  }

  UniqueString *getIdentifierForTokenKind(parser::TokenKind tokenKind) const {
    return context_.getStringTable()
        .getIdentifier(hermes::parser::tokenKindStr(tokenKind))
        .getUnderlyingPointer();
  }

  bool isSuperCtorCall(ESTree::Node *node) {
    auto *stmt = llvh::dyn_cast<ESTree::ExpressionStatementNode>(node);
    if (stmt == nullptr) {
      return false;
    }

    auto *call = llvh::dyn_cast<ESTree::CallExpressionNode>(stmt->_expression);
    if (call == nullptr) {
      return false;
    }
    auto *callee = ESTree::getCallee(call);
    return callee->getKind() == ESTree::NodeKind::Super;
  }

  void replaceThisCalls(ESTree::Node *node, bool newReplaceThis) {
    auto oldReplaceThis = replaceThis;
    replaceThis = newReplaceThis;
    visit(node);
    replaceThis = oldReplaceThis;
  }
};

void transformES6Classes(Context &context, ESTree::Node *node) {
  ES6ClassesTransformations transformations(context);
  visitESTreeNodeNoReplace(transformations, node);
}

} // namespace hermes
