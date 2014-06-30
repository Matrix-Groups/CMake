/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2012 Stephen Kelly <steveire@gmail.com>

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/
#include "cmMakefile.h"

#include "cmGeneratorExpressionEvaluator.h"
#include "cmGeneratorExpressionParser.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include "cmGeneratorExpression.h"
#include "cmLocalGenerator.h"
#include "cmSourceFile.h"

#include <cmsys/String.h>

#include <assert.h>
#include <errno.h>

//----------------------------------------------------------------------------
#if !defined(__SUNPRO_CC) || __SUNPRO_CC > 0x510
static
#endif
void reportError(cmGeneratorExpressionContext *context,
                        const std::string &expr, const std::string &result)
{
  context->HadError = true;
  if (context->Quiet)
    {
    return;
    }

  cmOStringStream e;
  e << "Error evaluating generator expression:\n"
    << "  " << expr << "\n"
    << result;
  context->Makefile->GetCMakeInstance()
    ->IssueMessage(cmake::FATAL_ERROR, e.str(),
                    context->Backtrace);
}

//----------------------------------------------------------------------------
struct cmGeneratorExpressionNode
{
  enum {
    DynamicParameters = 0,
    OneOrMoreParameters = -1,
    OneOrZeroParameters = -2
  };
  virtual ~cmGeneratorExpressionNode() {}

  virtual bool GeneratesContent() const { return true; }

  virtual bool RequiresLiteralInput() const { return false; }

  virtual bool AcceptsArbitraryContentParameter() const
    { return false; }

  virtual int NumExpectedParameters() const { return 1; }

  virtual std::string Evaluate(const std::vector<std::string> &parameters,
                               cmGeneratorExpressionContext *context,
                               const GeneratorExpressionContent *content,
                               cmGeneratorExpressionDAGChecker *dagChecker
                              ) const = 0;
};

//----------------------------------------------------------------------------
static const struct ZeroNode : public cmGeneratorExpressionNode
{
  ZeroNode() {}

  virtual bool GeneratesContent() const { return false; }

  virtual bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return std::string();
  }
} zeroNode;

//----------------------------------------------------------------------------
static const struct OneNode : public cmGeneratorExpressionNode
{
  OneNode() {}

  virtual bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return parameters.front();
  }
} oneNode;

//----------------------------------------------------------------------------
static const struct OneNode buildInterfaceNode;

//----------------------------------------------------------------------------
static const struct ZeroNode installInterfaceNode;

//----------------------------------------------------------------------------
#define BOOLEAN_OP_NODE(OPNAME, OP, SUCCESS_VALUE, FAILURE_VALUE) \
static const struct OP ## Node : public cmGeneratorExpressionNode \
{ \
  OP ## Node () {} \
  virtual int NumExpectedParameters() const { return OneOrMoreParameters; } \
 \
  std::string Evaluate(const std::vector<std::string> &parameters, \
                       cmGeneratorExpressionContext *context, \
                       const GeneratorExpressionContent *content, \
                       cmGeneratorExpressionDAGChecker *) const \
  { \
    std::vector<std::string>::const_iterator it = parameters.begin(); \
    const std::vector<std::string>::const_iterator end = parameters.end(); \
    for ( ; it != end; ++it) \
      { \
      if (*it == #FAILURE_VALUE) \
        { \
        return #FAILURE_VALUE; \
        } \
      else if (*it != #SUCCESS_VALUE) \
        { \
        reportError(context, content->GetOriginalExpression(), \
        "Parameters to $<" #OP "> must resolve to either '0' or '1'."); \
        return std::string(); \
        } \
      } \
    return #SUCCESS_VALUE; \
  } \
} OPNAME;

BOOLEAN_OP_NODE(andNode, AND, 1, 0)
BOOLEAN_OP_NODE(orNode, OR, 0, 1)

#undef BOOLEAN_OP_NODE

//----------------------------------------------------------------------------
static const struct NotNode : public cmGeneratorExpressionNode
{
  NotNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    if (*parameters.begin() != "0" && *parameters.begin() != "1")
      {
      reportError(context, content->GetOriginalExpression(),
            "$<NOT> parameter must resolve to exactly one '0' or '1' value.");
      return std::string();
      }
    return *parameters.begin() == "0" ? "1" : "0";
  }
} notNode;

//----------------------------------------------------------------------------
static const struct BoolNode : public cmGeneratorExpressionNode
{
  BoolNode() {}

  virtual int NumExpectedParameters() const { return 1; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return !cmSystemTools::IsOff(parameters.begin()->c_str()) ? "1" : "0";
  }
} boolNode;

//----------------------------------------------------------------------------
static const struct StrEqualNode : public cmGeneratorExpressionNode
{
  StrEqualNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return *parameters.begin() == parameters[1] ? "1" : "0";
  }
} strEqualNode;

//----------------------------------------------------------------------------
static const struct EqualNode : public cmGeneratorExpressionNode
{
  EqualNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    char *pEnd;

    int base = 0;
    bool flipSign = false;

    const char *lhs = parameters[0].c_str();
    if (cmHasLiteralPrefix(lhs, "0b") || cmHasLiteralPrefix(lhs, "0B"))
      {
      base = 2;
      lhs += 2;
      }
    if (cmHasLiteralPrefix(lhs, "-0b") || cmHasLiteralPrefix(lhs, "-0B"))
      {
      base = 2;
      lhs += 3;
      flipSign = true;
      }
    if (cmHasLiteralPrefix(lhs, "+0b") || cmHasLiteralPrefix(lhs, "+0B"))
      {
      base = 2;
      lhs += 3;
      }

    long lnum = strtol(lhs, &pEnd, base);
    if (pEnd == lhs || *pEnd != '\0' || errno == ERANGE)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<EQUAL> parameter " + parameters[0] + " is not a valid integer.");
      return std::string();
      }

    if (flipSign)
      {
      lnum = -lnum;
      }

    base = 0;
    flipSign = false;

    const char *rhs = parameters[1].c_str();
    if (cmHasLiteralPrefix(rhs, "0b") || cmHasLiteralPrefix(rhs, "0B"))
      {
      base = 2;
      rhs += 2;
      }
    if (cmHasLiteralPrefix(rhs, "-0b") || cmHasLiteralPrefix(rhs, "-0B"))
      {
      base = 2;
      rhs += 3;
      flipSign = true;
      }
    if (cmHasLiteralPrefix(rhs, "+0b") || cmHasLiteralPrefix(rhs, "+0B"))
      {
      base = 2;
      rhs += 3;
      }

    long rnum = strtol(rhs, &pEnd, base);
    if (pEnd == rhs || *pEnd != '\0' || errno == ERANGE)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<EQUAL> parameter " + parameters[1] + " is not a valid integer.");
      return std::string();
      }

    if (flipSign)
      {
      rnum = -rnum;
      }

    return lnum == rnum ? "1" : "0";
  }
} equalNode;

//----------------------------------------------------------------------------
static const struct LowerCaseNode : public cmGeneratorExpressionNode
{
  LowerCaseNode() {}

  bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::LowerCase(parameters.front());
  }
} lowerCaseNode;

//----------------------------------------------------------------------------
static const struct UpperCaseNode : public cmGeneratorExpressionNode
{
  UpperCaseNode() {}

  bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::UpperCase(parameters.front());
  }
} upperCaseNode;

//----------------------------------------------------------------------------
static const struct MakeCIdentifierNode : public cmGeneratorExpressionNode
{
  MakeCIdentifierNode() {}

  bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::MakeCidentifier(parameters.front().c_str());
  }
} makeCIdentifierNode;

//----------------------------------------------------------------------------
static const struct Angle_RNode : public cmGeneratorExpressionNode
{
  Angle_RNode() {}

  virtual int NumExpectedParameters() const { return 0; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return ">";
  }
} angle_rNode;

//----------------------------------------------------------------------------
static const struct CommaNode : public cmGeneratorExpressionNode
{
  CommaNode() {}

  virtual int NumExpectedParameters() const { return 0; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return ",";
  }
} commaNode;

//----------------------------------------------------------------------------
static const struct SemicolonNode : public cmGeneratorExpressionNode
{
  SemicolonNode() {}

  virtual int NumExpectedParameters() const { return 0; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return ";";
  }
} semicolonNode;

//----------------------------------------------------------------------------
struct CompilerIdNode : public cmGeneratorExpressionNode
{
  CompilerIdNode() {}

  virtual int NumExpectedParameters() const { return OneOrZeroParameters; }

  std::string EvaluateWithLanguage(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *,
                       const std::string &lang) const
  {
    const char *compilerId = context->Makefile ?
                              context->Makefile->GetSafeDefinition(
                                      "CMAKE_" + lang + "_COMPILER_ID") : "";
    if (parameters.size() == 0)
      {
      return compilerId ? compilerId : "";
      }
    static cmsys::RegularExpression compilerIdValidator("^[A-Za-z0-9_]*$");
    if (!compilerIdValidator.find(*parameters.begin()))
      {
      reportError(context, content->GetOriginalExpression(),
                  "Expression syntax not recognized.");
      return std::string();
      }
    if (!compilerId)
      {
      return parameters.front().empty() ? "1" : "0";
      }

    if (strcmp(parameters.begin()->c_str(), compilerId) == 0)
      {
      return "1";
      }

    if (cmsysString_strcasecmp(parameters.begin()->c_str(), compilerId) == 0)
      {
      switch(context->Makefile->GetPolicyStatus(cmPolicies::CMP0044))
        {
        case cmPolicies::WARN:
          {
          cmOStringStream e;
          e << context->Makefile->GetPolicies()
                      ->GetPolicyWarning(cmPolicies::CMP0044);
          context->Makefile->GetCMakeInstance()
                 ->IssueMessage(cmake::AUTHOR_WARNING,
                                e.str(), context->Backtrace);
          }
        case cmPolicies::OLD:
          return "1";
        case cmPolicies::NEW:
        case cmPolicies::REQUIRED_ALWAYS:
        case cmPolicies::REQUIRED_IF_USED:
          break;
        }
      }
    return "0";
  }
};

//----------------------------------------------------------------------------
static const struct CCompilerIdNode : public CompilerIdNode
{
  CCompilerIdNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    if (!context->HeadTarget)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<C_COMPILER_ID> may only be used with binary targets.  It may "
          "not be used with add_custom_command or add_custom_target.");
      return std::string();
      }
    return this->EvaluateWithLanguage(parameters, context, content,
                                      dagChecker, "C");
  }
} cCompilerIdNode;

//----------------------------------------------------------------------------
static const struct CXXCompilerIdNode : public CompilerIdNode
{
  CXXCompilerIdNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    if (!context->HeadTarget)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<CXX_COMPILER_ID> may only be used with binary targets.  It may "
          "not be used with add_custom_command or add_custom_target.");
      return std::string();
      }
    return this->EvaluateWithLanguage(parameters, context, content,
                                      dagChecker, "CXX");
  }
} cxxCompilerIdNode;

//----------------------------------------------------------------------------
struct CompilerVersionNode : public cmGeneratorExpressionNode
{
  CompilerVersionNode() {}

  virtual int NumExpectedParameters() const { return OneOrZeroParameters; }

  std::string EvaluateWithLanguage(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *,
                       const std::string &lang) const
  {
    const char *compilerVersion = context->Makefile ?
                              context->Makefile->GetSafeDefinition(
                                  "CMAKE_" + lang + "_COMPILER_VERSION") : "";
    if (parameters.size() == 0)
      {
      return compilerVersion ? compilerVersion : "";
      }

    static cmsys::RegularExpression compilerIdValidator("^[0-9\\.]*$");
    if (!compilerIdValidator.find(*parameters.begin()))
      {
      reportError(context, content->GetOriginalExpression(),
                  "Expression syntax not recognized.");
      return std::string();
      }
    if (!compilerVersion)
      {
      return parameters.front().empty() ? "1" : "0";
      }

    return cmSystemTools::VersionCompare(cmSystemTools::OP_EQUAL,
                                      parameters.begin()->c_str(),
                                      compilerVersion) ? "1" : "0";
  }
};

//----------------------------------------------------------------------------
static const struct CCompilerVersionNode : public CompilerVersionNode
{
  CCompilerVersionNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    if (!context->HeadTarget)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<C_COMPILER_VERSION> may only be used with binary targets.  It "
          "may not be used with add_custom_command or add_custom_target.");
      return std::string();
      }
    return this->EvaluateWithLanguage(parameters, context, content,
                                      dagChecker, "C");
  }
} cCompilerVersionNode;

//----------------------------------------------------------------------------
static const struct CxxCompilerVersionNode : public CompilerVersionNode
{
  CxxCompilerVersionNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    if (!context->HeadTarget)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<CXX_COMPILER_VERSION> may only be used with binary targets.  It "
          "may not be used with add_custom_command or add_custom_target.");
      return std::string();
      }
    return this->EvaluateWithLanguage(parameters, context, content,
                                      dagChecker, "CXX");
  }
} cxxCompilerVersionNode;


//----------------------------------------------------------------------------
struct PlatformIdNode : public cmGeneratorExpressionNode
{
  PlatformIdNode() {}

  virtual int NumExpectedParameters() const { return OneOrZeroParameters; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    const char *platformId = context->Makefile ?
                              context->Makefile->GetSafeDefinition(
                        "CMAKE_SYSTEM_NAME") : "";
    if (parameters.size() == 0)
      {
      return platformId ? platformId : "";
      }

    if (!platformId)
      {
      return parameters.front().empty() ? "1" : "0";
      }

    if (strcmp(parameters.begin()->c_str(), platformId) == 0)
      {
      return "1";
      }
    return "0";
  }
} platformIdNode;

//----------------------------------------------------------------------------
static const struct VersionGreaterNode : public cmGeneratorExpressionNode
{
  VersionGreaterNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::VersionCompare(cmSystemTools::OP_GREATER,
                                         parameters.front().c_str(),
                                         parameters[1].c_str()) ? "1" : "0";
  }
} versionGreaterNode;

//----------------------------------------------------------------------------
static const struct VersionLessNode : public cmGeneratorExpressionNode
{
  VersionLessNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::VersionCompare(cmSystemTools::OP_LESS,
                                         parameters.front().c_str(),
                                         parameters[1].c_str()) ? "1" : "0";
  }
} versionLessNode;

//----------------------------------------------------------------------------
static const struct VersionEqualNode : public cmGeneratorExpressionNode
{
  VersionEqualNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return cmSystemTools::VersionCompare(cmSystemTools::OP_EQUAL,
                                         parameters.front().c_str(),
                                         parameters[1].c_str()) ? "1" : "0";
  }
} versionEqualNode;

//----------------------------------------------------------------------------
static const struct LinkOnlyNode : public cmGeneratorExpressionNode
{
  LinkOnlyNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    if(!dagChecker->GetTransitivePropertiesOnly())
      {
      return parameters.front();
      }
    return "";
  }
} linkOnlyNode;

//----------------------------------------------------------------------------
static const struct ConfigurationNode : public cmGeneratorExpressionNode
{
  ConfigurationNode() {}

  virtual int NumExpectedParameters() const { return 0; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    context->HadContextSensitiveCondition = true;
    return context->Config;
  }
} configurationNode;

//----------------------------------------------------------------------------
static const struct ConfigurationTestNode : public cmGeneratorExpressionNode
{
  ConfigurationTestNode() {}

  virtual int NumExpectedParameters() const { return OneOrZeroParameters; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    if (parameters.empty())
      {
      return configurationNode.Evaluate(parameters, context, content, 0);
      }
    static cmsys::RegularExpression configValidator("^[A-Za-z0-9_]*$");
    if (!configValidator.find(*parameters.begin()))
      {
      reportError(context, content->GetOriginalExpression(),
                  "Expression syntax not recognized.");
      return std::string();
      }
    context->HadContextSensitiveCondition = true;
    if (context->Config.empty())
      {
      return parameters.front().empty() ? "1" : "0";
      }

    if (cmsysString_strcasecmp(parameters.begin()->c_str(),
                               context->Config.c_str()) == 0)
      {
      return "1";
      }

    if (context->CurrentTarget
        && context->CurrentTarget->IsImported())
      {
      const char* loc = 0;
      const char* imp = 0;
      std::string suffix;
      if (context->CurrentTarget->GetMappedConfig(context->Config,
                                                  &loc,
                                                  &imp,
                                                  suffix))
        {
        // This imported target has an appropriate location
        // for this (possibly mapped) config.
        // Check if there is a proper config mapping for the tested config.
        std::vector<std::string> mappedConfigs;
        std::string mapProp = "MAP_IMPORTED_CONFIG_";
        mapProp += cmSystemTools::UpperCase(context->Config);
        if(const char* mapValue =
                        context->CurrentTarget->GetProperty(mapProp))
          {
          cmSystemTools::ExpandListArgument(cmSystemTools::UpperCase(mapValue),
                                            mappedConfigs);
          return std::find(mappedConfigs.begin(), mappedConfigs.end(),
                           cmSystemTools::UpperCase(parameters.front()))
              != mappedConfigs.end() ? "1" : "0";
          }
        }
      }
    return "0";
  }
} configurationTestNode;

static const struct JoinNode : public cmGeneratorExpressionNode
{
  JoinNode() {}

  virtual int NumExpectedParameters() const { return 2; }

  virtual bool AcceptsArbitraryContentParameter() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    std::string result;

    std::vector<std::string> list;
    cmSystemTools::ExpandListArgument(parameters.front(), list);
    std::string sep;
    for(std::vector<std::string>::const_iterator li = list.begin();
      li != list.end(); ++li)
      {
      result += sep + *li;
      sep = parameters[1];
      }
    return result;
  }
} joinNode;

#define TRANSITIVE_PROPERTY_NAME(PROPERTY) \
  , "INTERFACE_" #PROPERTY

//----------------------------------------------------------------------------
static const char* targetPropertyTransitiveWhitelist[] = {
  0
  CM_FOR_EACH_TRANSITIVE_PROPERTY_NAME(TRANSITIVE_PROPERTY_NAME)
};

#undef TRANSITIVE_PROPERTY_NAME

std::string
getLinkedTargetsContent(
  std::vector<cmTarget const*> &targets,
  cmTarget const* target,
  cmTarget const* headTarget,
  cmGeneratorExpressionContext *context,
  cmGeneratorExpressionDAGChecker *dagChecker,
  const std::string &interfacePropertyName)
{
  cmGeneratorExpression ge(&context->Backtrace);

  std::string sep;
  std::string depString;
  for (std::vector<cmTarget const*>::const_iterator
      it = targets.begin();
      it != targets.end(); ++it)
    {
    if (*it == target)
      {
      // Broken code can have a target in its own link interface.
      // Don't follow such link interface entries so as not to create a
      // self-referencing loop.
      continue;
      }
    depString +=
      sep + "$<TARGET_PROPERTY:" +
        (*it)->GetName() + "," + interfacePropertyName + ">";
    sep = ";";
    }
  cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(depString);
  std::string linkedTargetsContent = cge->Evaluate(target->GetMakefile(),
                      context->Config,
                      context->Quiet,
                      headTarget,
                      target,
                      dagChecker);
  if (cge->GetHadContextSensitiveCondition())
    {
    context->HadContextSensitiveCondition = true;
    }
  return linkedTargetsContent;
}

std::string
getLinkedTargetsContent(
  std::vector<cmLinkImplItem> const &libraries,
  cmTarget const* target,
  cmTarget const* headTarget,
  cmGeneratorExpressionContext *context,
  cmGeneratorExpressionDAGChecker *dagChecker,
  const std::string &interfacePropertyName)
{
  std::vector<cmTarget const*> tgts;
  for (std::vector<cmLinkImplItem>::const_iterator
      it = libraries.begin();
      it != libraries.end(); ++it)
    {
    if (it->Target)
      {
      tgts.push_back(it->Target);
      }
    }
  return getLinkedTargetsContent(tgts, target, headTarget, context,
                                 dagChecker, interfacePropertyName);
}

//----------------------------------------------------------------------------
static const struct TargetPropertyNode : public cmGeneratorExpressionNode
{
  TargetPropertyNode() {}

  // This node handles errors on parameter count itself.
  virtual int NumExpectedParameters() const { return OneOrMoreParameters; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagCheckerParent
                      ) const
  {
    if (parameters.size() != 1 && parameters.size() != 2)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<TARGET_PROPERTY:...> expression requires one or two parameters");
      return std::string();
      }
    static cmsys::RegularExpression propertyNameValidator("^[A-Za-z0-9_]+$");

    cmTarget const* target = context->HeadTarget;
    std::string propertyName = *parameters.begin();

    if (!target && parameters.size() == 1)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<TARGET_PROPERTY:prop>  may only be used with binary targets.  "
          "It may not be used with add_custom_command or add_custom_target.  "
          "Specify the target to read a property from using the "
          "$<TARGET_PROPERTY:tgt,prop> signature instead.");
      return std::string();
      }

    if (parameters.size() == 2)
      {
      if (parameters.begin()->empty() && parameters[1].empty())
        {
        reportError(context, content->GetOriginalExpression(),
            "$<TARGET_PROPERTY:tgt,prop> expression requires a non-empty "
            "target name and property name.");
        return std::string();
        }
      if (parameters.begin()->empty())
        {
        reportError(context, content->GetOriginalExpression(),
            "$<TARGET_PROPERTY:tgt,prop> expression requires a non-empty "
            "target name.");
        return std::string();
        }

      std::string targetName = parameters.front();
      propertyName = parameters[1];
      if (!cmGeneratorExpression::IsValidTargetName(targetName))
        {
        if (!propertyNameValidator.find(propertyName.c_str()))
          {
          ::reportError(context, content->GetOriginalExpression(),
                        "Target name and property name not supported.");
          return std::string();
          }
        ::reportError(context, content->GetOriginalExpression(),
                      "Target name not supported.");
        return std::string();
        }
      if(propertyName == "ALIASED_TARGET")
        {
        if(context->Makefile->IsAlias(targetName))
          {
          if(cmTarget* tgt = context->Makefile->FindTargetToUse(targetName))
            {
            return tgt->GetName();
            }
          }
        return "";
        }
      target = context->Makefile->FindTargetToUse(targetName);

      if (!target)
        {
        cmOStringStream e;
        e << "Target \""
          << targetName
          << "\" not found.";
        reportError(context, content->GetOriginalExpression(), e.str());
        return std::string();
        }
        context->AllTargets.insert(target);
      }

    if (target == context->HeadTarget)
      {
      // Keep track of the properties seen while processing.
      // The evaluation of the LINK_LIBRARIES generator expressions
      // will check this to ensure that properties have one consistent
      // value for all evaluations.
      context->SeenTargetProperties.insert(propertyName);
      }

    if (propertyName.empty())
      {
      reportError(context, content->GetOriginalExpression(),
          "$<TARGET_PROPERTY:...> expression requires a non-empty property "
          "name.");
      return std::string();
      }

    if (!propertyNameValidator.find(propertyName))
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "Property name not supported.");
      return std::string();
      }

    assert(target);

    if (propertyName == "LINKER_LANGUAGE")
      {
      if (target->LinkLanguagePropagatesToDependents() &&
          dagCheckerParent && (dagCheckerParent->EvaluatingLinkLibraries()
            || dagCheckerParent->EvaluatingSources()))
        {
        reportError(context, content->GetOriginalExpression(),
            "LINKER_LANGUAGE target property can not be used while evaluating "
            "link libraries for a static library");
        return std::string();
        }
      return target->GetLinkerLanguage(context->Config);
      }

    cmGeneratorExpressionDAGChecker dagChecker(context->Backtrace,
                                               target->GetName(),
                                               propertyName,
                                               content,
                                               dagCheckerParent);

    switch (dagChecker.Check())
      {
    case cmGeneratorExpressionDAGChecker::SELF_REFERENCE:
      dagChecker.ReportError(context, content->GetOriginalExpression());
      return std::string();
    case cmGeneratorExpressionDAGChecker::CYCLIC_REFERENCE:
      // No error. We just skip cyclic references.
      return std::string();
    case cmGeneratorExpressionDAGChecker::ALREADY_SEEN:
      for (size_t i = 1;
          i < cmArraySize(targetPropertyTransitiveWhitelist);
          ++i)
        {
        if (targetPropertyTransitiveWhitelist[i] == propertyName)
          {
          // No error. We're not going to find anything new here.
          return std::string();
          }
        }
    case cmGeneratorExpressionDAGChecker::DAG:
      break;
      }

    const char *prop = target->GetProperty(propertyName);

    if (dagCheckerParent)
      {
      if (dagCheckerParent->EvaluatingLinkLibraries())
        {
#define TRANSITIVE_PROPERTY_COMPARE(PROPERTY) \
    (#PROPERTY == propertyName || "INTERFACE_" #PROPERTY == propertyName) ||
        if (CM_FOR_EACH_TRANSITIVE_PROPERTY_NAME(TRANSITIVE_PROPERTY_COMPARE)
            false)
          {
          reportError(context, content->GetOriginalExpression(),
              "$<TARGET_PROPERTY:...> expression in link libraries "
              "evaluation depends on target property which is transitive "
              "over the link libraries, creating a recursion.");
          return std::string();
          }
#undef TRANSITIVE_PROPERTY_COMPARE

        if(!prop)
          {
          return std::string();
          }
        }
      else
        {
#define ASSERT_TRANSITIVE_PROPERTY_METHOD(METHOD) \
  dagCheckerParent->METHOD () ||

        assert(
          CM_FOR_EACH_TRANSITIVE_PROPERTY_METHOD(
                                            ASSERT_TRANSITIVE_PROPERTY_METHOD)
          false);
        }
#undef ASSERT_TRANSITIVE_PROPERTY_METHOD
      }

    std::string linkedTargetsContent;

    std::string interfacePropertyName;

#define POPULATE_INTERFACE_PROPERTY_NAME(prop) \
    if (propertyName == #prop || propertyName == "INTERFACE_" #prop) \
      { \
      interfacePropertyName = "INTERFACE_" #prop; \
      } \
    else

    CM_FOR_EACH_TRANSITIVE_PROPERTY_NAME(POPULATE_INTERFACE_PROPERTY_NAME)
      // Note that the above macro terminates with an else
    /* else */ if (cmHasLiteralPrefix(propertyName.c_str(),
                           "COMPILE_DEFINITIONS_"))
      {
      cmPolicies::PolicyStatus polSt =
                      context->Makefile->GetPolicyStatus(cmPolicies::CMP0043);
      if (polSt == cmPolicies::WARN || polSt == cmPolicies::OLD)
        {
        interfacePropertyName = "INTERFACE_COMPILE_DEFINITIONS";
        }
      }
#undef POPULATE_INTERFACE_PROPERTY_NAME

    cmTarget const* headTarget = context->HeadTarget
                               ? context->HeadTarget : target;

    const char * const *transBegin =
                        cmArrayBegin(targetPropertyTransitiveWhitelist) + 1;
    const char * const *transEnd =
                        cmArrayEnd(targetPropertyTransitiveWhitelist);

    if (std::find_if(transBegin, transEnd,
                     cmStrCmp(propertyName)) != transEnd)
      {

      std::vector<cmTarget const*> tgts;
      target->GetTransitivePropertyTargets(context->Config,
                                                 headTarget, tgts);
      if (!tgts.empty())
        {
        linkedTargetsContent =
                  getLinkedTargetsContent(tgts, target,
                                          headTarget,
                                          context, &dagChecker,
                                          interfacePropertyName);
        }
      }
    else if (std::find_if(transBegin, transEnd,
                          cmStrCmp(interfacePropertyName)) != transEnd)
      {
      const cmTarget::LinkImplementation *impl
        = target->GetLinkImplementationLibraries(context->Config);
      if(impl)
        {
        linkedTargetsContent =
                  getLinkedTargetsContent(impl->Libraries, target,
                                          headTarget,
                                          context, &dagChecker,
                                          interfacePropertyName);
        }
      }

    linkedTargetsContent =
          cmGeneratorExpression::StripEmptyListElements(linkedTargetsContent);

    if (!prop)
      {
      if (target->IsImported()
          || target->GetType() == cmTarget::INTERFACE_LIBRARY)
        {
        return linkedTargetsContent;
        }
      if (target->IsLinkInterfaceDependentBoolProperty(propertyName,
                                                       context->Config))
        {
        context->HadContextSensitiveCondition = true;
        return target->GetLinkInterfaceDependentBoolProperty(
                                                propertyName,
                                                context->Config) ? "1" : "0";
        }
      if (target->IsLinkInterfaceDependentStringProperty(propertyName,
                                                         context->Config))
        {
        context->HadContextSensitiveCondition = true;
        const char *propContent =
                              target->GetLinkInterfaceDependentStringProperty(
                                                propertyName,
                                                context->Config);
        return propContent ? propContent : "";
        }
      if (target->IsLinkInterfaceDependentNumberMinProperty(propertyName,
                                                         context->Config))
        {
        context->HadContextSensitiveCondition = true;
        const char *propContent =
                          target->GetLinkInterfaceDependentNumberMinProperty(
                                                propertyName,
                                                context->Config);
        return propContent ? propContent : "";
        }
      if (target->IsLinkInterfaceDependentNumberMaxProperty(propertyName,
                                                         context->Config))
        {
        context->HadContextSensitiveCondition = true;
        const char *propContent =
                          target->GetLinkInterfaceDependentNumberMaxProperty(
                                                propertyName,
                                                context->Config);
        return propContent ? propContent : "";
        }

      return linkedTargetsContent;
      }

    if (!target->IsImported()
        && dagCheckerParent && !dagCheckerParent->EvaluatingLinkLibraries())
      {
      if (target->IsLinkInterfaceDependentNumberMinProperty(propertyName,
                                                        context->Config))
        {
        context->HadContextSensitiveCondition = true;
        const char *propContent =
                            target->GetLinkInterfaceDependentNumberMinProperty(
                                                propertyName,
                                                context->Config);
        return propContent ? propContent : "";
        }
      if (target->IsLinkInterfaceDependentNumberMaxProperty(propertyName,
                                                        context->Config))
        {
        context->HadContextSensitiveCondition = true;
        const char *propContent =
                            target->GetLinkInterfaceDependentNumberMaxProperty(
                                                propertyName,
                                                context->Config);
        return propContent ? propContent : "";
        }
      }
    for (size_t i = 1;
         i < cmArraySize(targetPropertyTransitiveWhitelist);
         ++i)
      {
      if (targetPropertyTransitiveWhitelist[i] == interfacePropertyName)
        {
        cmGeneratorExpression ge(&context->Backtrace);
        cmsys::auto_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(prop);
        std::string result = cge->Evaluate(context->Makefile,
                            context->Config,
                            context->Quiet,
                            headTarget,
                            target,
                            &dagChecker);

        if (cge->GetHadContextSensitiveCondition())
          {
          context->HadContextSensitiveCondition = true;
          }
        if (!linkedTargetsContent.empty())
          {
          result += (result.empty() ? "" : ";") + linkedTargetsContent;
          }
        return result;
        }
      }
    return prop;
  }
} targetPropertyNode;

//----------------------------------------------------------------------------
static const struct TargetNameNode : public cmGeneratorExpressionNode
{
  TargetNameNode() {}

  virtual bool GeneratesContent() const { return true; }

  virtual bool AcceptsArbitraryContentParameter() const { return true; }
  virtual bool RequiresLiteralInput() const { return true; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *,
                       const GeneratorExpressionContent *,
                       cmGeneratorExpressionDAGChecker *) const
  {
    return parameters.front();
  }

  virtual int NumExpectedParameters() const { return 1; }

} targetNameNode;

//----------------------------------------------------------------------------
static const struct TargetObjectsNode : public cmGeneratorExpressionNode
{
  TargetObjectsNode() {}

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    if (!context->EvaluateForBuildsystem)
      {
      cmOStringStream e;
      e << "The evaluation of the TARGET_OBJECTS generator expression "
        "is only suitable for consumption by CMake.  It is not suitable "
        "for writing out elsewhere.";
      reportError(context, content->GetOriginalExpression(), e.str());
      return std::string();
      }

    std::string tgtName = parameters.front();
    cmGeneratorTarget* gt =
                context->Makefile->FindGeneratorTargetToUse(tgtName.c_str());
    if (!gt)
      {
      cmOStringStream e;
      e << "Objects of target \"" << tgtName
        << "\" referenced but no such target exists.";
      reportError(context, content->GetOriginalExpression(), e.str());
      return std::string();
      }
    if (gt->GetType() != cmTarget::OBJECT_LIBRARY)
      {
      cmOStringStream e;
      e << "Objects of target \"" << tgtName
        << "\" referenced but is not an OBJECT library.";
      reportError(context, content->GetOriginalExpression(), e.str());
      return std::string();
      }

    std::vector<cmSourceFile const*> objectSources;
    gt->GetObjectSources(objectSources, context->Config);
    std::map<cmSourceFile const*, std::string> mapping;

    for(std::vector<cmSourceFile const*>::const_iterator it
        = objectSources.begin(); it != objectSources.end(); ++it)
      {
      mapping[*it];
      }

    gt->LocalGenerator->ComputeObjectFilenames(mapping, gt);

    std::string obj_dir = gt->ObjectDirectory;
    std::string result;
    const char* sep = "";
    for(std::map<cmSourceFile const*, std::string>::const_iterator it
        = mapping.begin(); it != mapping.end(); ++it)
      {
      assert(!it->second.empty());
      result += sep;
      std::string objFile = obj_dir + it->second;
      cmSourceFile* sf = context->Makefile->GetOrCreateSource(objFile, true);
      sf->SetObjectLibrary(tgtName);
      sf->SetProperty("EXTERNAL_OBJECT", "1");
      result += objFile;
      sep = ";";
      }
    return result;
  }
} targetObjectsNode;

//----------------------------------------------------------------------------
static const struct CompileFeaturesNode : public cmGeneratorExpressionNode
{
  CompileFeaturesNode() {}

  virtual int NumExpectedParameters() const { return OneOrMoreParameters; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    cmTarget const* target = context->HeadTarget;
    if (!target)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<COMPILE_FEATURE> may only be used with binary targets.  It may "
          "not be used with add_custom_command or add_custom_target.");
      return std::string();
      }

    typedef std::map<std::string, std::vector<std::string> > LangMap;
    static LangMap availableFeatures;

    LangMap testedFeatures;

    for (std::vector<std::string>::const_iterator it = parameters.begin();
        it != parameters.end(); ++it)
      {
      std::string error;
      std::string lang;
      if (!context->Makefile->CompileFeatureKnown(context->HeadTarget,
                                                  *it, lang, &error))
        {
        reportError(context, content->GetOriginalExpression(), error);
        return std::string();
        }
      testedFeatures[lang].push_back(*it);

      if (availableFeatures.find(lang) == availableFeatures.end())
        {
        const char* featuresKnown
                  = context->Makefile->CompileFeaturesAvailable(lang, &error);
        if (!featuresKnown)
          {
          reportError(context, content->GetOriginalExpression(), error);
          return std::string();
          }
        cmSystemTools::ExpandListArgument(featuresKnown,
                                          availableFeatures[lang]);
        }
      }

    bool evalLL = dagChecker && dagChecker->EvaluatingLinkLibraries();

    std::string result;

    for (LangMap::const_iterator lit = testedFeatures.begin();
          lit != testedFeatures.end(); ++lit)
      {
      for (std::vector<std::string>::const_iterator it = lit->second.begin();
          it != lit->second.end(); ++it)
        {
        if (!context->Makefile->HaveFeatureAvailable(target,
                                                      lit->first, *it))
          {
          if (evalLL)
            {
            const char* l = target->GetProperty(lit->first + "_STANDARD");
            if (!l)
              {
              l = context->Makefile
                  ->GetDefinition("CMAKE_" + lit->first + "_STANDARD_DEFAULT");
              }
            assert(l);
            context->MaxLanguageStandard[target][lit->first] = l;
            }
          else
            {
            return "0";
            }
          }
        }
      }
    return "1";
  }
} compileFeaturesNode;

//----------------------------------------------------------------------------
static const char* targetPolicyWhitelist[] = {
  0
#define TARGET_POLICY_STRING(POLICY) \
  , #POLICY

  CM_FOR_EACH_TARGET_POLICY(TARGET_POLICY_STRING)

#undef TARGET_POLICY_STRING
};

cmPolicies::PolicyStatus statusForTarget(cmTarget const* tgt,
                                         const char *policy)
{
#define RETURN_POLICY(POLICY) \
  if (strcmp(policy, #POLICY) == 0) \
  { \
    return tgt->GetPolicyStatus ## POLICY (); \
  } \

  CM_FOR_EACH_TARGET_POLICY(RETURN_POLICY)

#undef RETURN_POLICY

  assert("!Unreachable code. Not a valid policy");
  return cmPolicies::WARN;
}

cmPolicies::PolicyID policyForString(const char *policy_id)
{
#define RETURN_POLICY_ID(POLICY_ID) \
  if (strcmp(policy_id, #POLICY_ID) == 0) \
  { \
    return cmPolicies:: POLICY_ID; \
  } \

  CM_FOR_EACH_TARGET_POLICY(RETURN_POLICY_ID)

#undef RETURN_POLICY_ID

  assert("!Unreachable code. Not a valid policy");
  return cmPolicies::CMP0002;
}

//----------------------------------------------------------------------------
static const struct TargetPolicyNode : public cmGeneratorExpressionNode
{
  TargetPolicyNode() {}

  virtual int NumExpectedParameters() const { return 1; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context ,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    if (!context->HeadTarget)
      {
      reportError(context, content->GetOriginalExpression(),
          "$<TARGET_POLICY:prop> may only be used with binary targets.  It "
          "may not be used with add_custom_command or add_custom_target.");
      return std::string();
      }

    context->HadContextSensitiveCondition = true;

    for (size_t i = 1; i < cmArraySize(targetPolicyWhitelist); ++i)
      {
      const char *policy = targetPolicyWhitelist[i];
      if (parameters.front() == policy)
        {
        cmMakefile *mf = context->HeadTarget->GetMakefile();
        switch(statusForTarget(context->HeadTarget, policy))
          {
          case cmPolicies::WARN:
            mf->IssueMessage(cmake::AUTHOR_WARNING,
                             mf->GetPolicies()->
                             GetPolicyWarning(policyForString(policy)));
          case cmPolicies::REQUIRED_IF_USED:
          case cmPolicies::REQUIRED_ALWAYS:
          case cmPolicies::OLD:
            return "0";
          case cmPolicies::NEW:
            return "1";
          }
        }
      }
    reportError(context, content->GetOriginalExpression(),
      "$<TARGET_POLICY:prop> may only be used with a limited number of "
      "policies.  Currently it may be used with the following policies:\n"

#define STRINGIFY_HELPER(X) #X
#define STRINGIFY(X) STRINGIFY_HELPER(X)

#define TARGET_POLICY_LIST_ITEM(POLICY) \
      " * " STRINGIFY(POLICY) "\n"

      CM_FOR_EACH_TARGET_POLICY(TARGET_POLICY_LIST_ITEM)

#undef TARGET_POLICY_LIST_ITEM
      );
    return std::string();
  }

} targetPolicyNode;

//----------------------------------------------------------------------------
static const struct InstallPrefixNode : public cmGeneratorExpressionNode
{
  InstallPrefixNode() {}

  virtual bool GeneratesContent() const { return true; }
  virtual int NumExpectedParameters() const { return 0; }

  std::string Evaluate(const std::vector<std::string> &,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *) const
  {
    reportError(context, content->GetOriginalExpression(),
                "INSTALL_PREFIX is a marker for install(EXPORT) only.  It "
                "should never be evaluated.");
    return std::string();
  }

} installPrefixNode;

//----------------------------------------------------------------------------
template<bool linker, bool soname>
struct TargetFilesystemArtifactResultCreator
{
  static std::string Create(cmTarget* target,
                            cmGeneratorExpressionContext *context,
                            const GeneratorExpressionContent *content);
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultCreator<false, true>
{
  static std::string Create(cmTarget* target,
                            cmGeneratorExpressionContext *context,
                            const GeneratorExpressionContent *content)
  {
    // The target soname file (.so.1).
    if(target->IsDLLPlatform())
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "TARGET_SONAME_FILE is not allowed "
                    "for DLL target platforms.");
      return std::string();
      }
    if(target->GetType() != cmTarget::SHARED_LIBRARY)
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "TARGET_SONAME_FILE is allowed only for "
                    "SHARED libraries.");
      return std::string();
      }
    std::string result = target->GetDirectory(context->Config);
    result += "/";
    result += target->GetSOName(context->Config);
    return result;
  }
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultCreator<true, false>
{
  static std::string Create(cmTarget* target,
                            cmGeneratorExpressionContext *context,
                            const GeneratorExpressionContent *content)
  {
    // The file used to link to the target (.so, .lib, .a).
    if(!target->IsLinkable())
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "TARGET_LINKER_FILE is allowed only for libraries and "
                    "executables with ENABLE_EXPORTS.");
      return std::string();
      }
    return target->GetFullPath(context->Config,
                               target->HasImportLibrary());
  }
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultCreator<false, false>
{
  static std::string Create(cmTarget* target,
                            cmGeneratorExpressionContext *context,
                            const GeneratorExpressionContent *)
  {
    return target->GetFullPath(context->Config, false, true);
  }
};


//----------------------------------------------------------------------------
template<bool dirQual, bool nameQual>
struct TargetFilesystemArtifactResultGetter
{
  static std::string Get(const std::string &result);
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultGetter<false, true>
{
  static std::string Get(const std::string &result)
  { return cmSystemTools::GetFilenameName(result); }
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultGetter<true, false>
{
  static std::string Get(const std::string &result)
  { return cmSystemTools::GetFilenamePath(result); }
};

//----------------------------------------------------------------------------
template<>
struct TargetFilesystemArtifactResultGetter<false, false>
{
  static std::string Get(const std::string &result)
  { return result; }
};

//----------------------------------------------------------------------------
template<bool linker, bool soname, bool dirQual, bool nameQual>
struct TargetFilesystemArtifact : public cmGeneratorExpressionNode
{
  TargetFilesystemArtifact() {}

  virtual int NumExpectedParameters() const { return 1; }

  std::string Evaluate(const std::vector<std::string> &parameters,
                       cmGeneratorExpressionContext *context,
                       const GeneratorExpressionContent *content,
                       cmGeneratorExpressionDAGChecker *dagChecker) const
  {
    // Lookup the referenced target.
    std::string name = *parameters.begin();

    if (!cmGeneratorExpression::IsValidTargetName(name))
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "Expression syntax not recognized.");
      return std::string();
      }
    cmTarget* target = context->Makefile->FindTargetToUse(name);
    if(!target)
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "No target \"" + name + "\"");
      return std::string();
      }
    if(target->GetType() >= cmTarget::OBJECT_LIBRARY &&
      target->GetType() != cmTarget::UNKNOWN_LIBRARY)
      {
      ::reportError(context, content->GetOriginalExpression(),
                  "Target \"" + name + "\" is not an executable or library.");
      return std::string();
      }
    if (dagChecker && (dagChecker->EvaluatingLinkLibraries(name.c_str())
        || (dagChecker->EvaluatingSources()
          && name == dagChecker->TopTarget())))
      {
      ::reportError(context, content->GetOriginalExpression(),
                    "Expressions which require the linker language may not "
                    "be used while evaluating link libraries");
      return std::string();
      }
    context->DependTargets.insert(target);
    context->AllTargets.insert(target);

    std::string result =
                TargetFilesystemArtifactResultCreator<linker, soname>::Create(
                          target,
                          context,
                          content);
    if (context->HadError)
      {
      return std::string();
      }
    return
        TargetFilesystemArtifactResultGetter<dirQual, nameQual>::Get(result);
  }
};

//----------------------------------------------------------------------------
static const
TargetFilesystemArtifact<false, false, false, false> targetFileNode;
static const
TargetFilesystemArtifact<true, false, false, false> targetLinkerFileNode;
static const
TargetFilesystemArtifact<false, true, false, false> targetSoNameFileNode;
static const
TargetFilesystemArtifact<false, false, false, true> targetFileNameNode;
static const
TargetFilesystemArtifact<true, false, false, true> targetLinkerFileNameNode;
static const
TargetFilesystemArtifact<false, true, false, true> targetSoNameFileNameNode;
static const
TargetFilesystemArtifact<false, false, true, false> targetFileDirNode;
static const
TargetFilesystemArtifact<true, false, true, false> targetLinkerFileDirNode;
static const
TargetFilesystemArtifact<false, true, true, false> targetSoNameFileDirNode;

//----------------------------------------------------------------------------
static const
cmGeneratorExpressionNode* GetNode(const std::string &identifier)
{
  typedef std::map<std::string, const cmGeneratorExpressionNode*> NodeMap;
  static NodeMap nodeMap;
  if (nodeMap.empty())
    {
    nodeMap["0"] = &zeroNode;
    nodeMap["1"] = &oneNode;
    nodeMap["AND"] = &andNode;
    nodeMap["OR"] = &orNode;
    nodeMap["NOT"] = &notNode;
    nodeMap["C_COMPILER_ID"] = &cCompilerIdNode;
    nodeMap["CXX_COMPILER_ID"] = &cxxCompilerIdNode;
    nodeMap["VERSION_GREATER"] = &versionGreaterNode;
    nodeMap["VERSION_LESS"] = &versionLessNode;
    nodeMap["VERSION_EQUAL"] = &versionEqualNode;
    nodeMap["C_COMPILER_VERSION"] = &cCompilerVersionNode;
    nodeMap["CXX_COMPILER_VERSION"] = &cxxCompilerVersionNode;
    nodeMap["PLATFORM_ID"] = &platformIdNode;
    nodeMap["COMPILE_FEATURES"] = &compileFeaturesNode;
    nodeMap["CONFIGURATION"] = &configurationNode;
    nodeMap["CONFIG"] = &configurationTestNode;
    nodeMap["TARGET_FILE"] = &targetFileNode;
    nodeMap["TARGET_LINKER_FILE"] = &targetLinkerFileNode;
    nodeMap["TARGET_SONAME_FILE"] = &targetSoNameFileNode;
    nodeMap["TARGET_FILE_NAME"] = &targetFileNameNode;
    nodeMap["TARGET_LINKER_FILE_NAME"] = &targetLinkerFileNameNode;
    nodeMap["TARGET_SONAME_FILE_NAME"] = &targetSoNameFileNameNode;
    nodeMap["TARGET_FILE_DIR"] = &targetFileDirNode;
    nodeMap["TARGET_LINKER_FILE_DIR"] = &targetLinkerFileDirNode;
    nodeMap["TARGET_SONAME_FILE_DIR"] = &targetSoNameFileDirNode;
    nodeMap["STREQUAL"] = &strEqualNode;
    nodeMap["EQUAL"] = &equalNode;
    nodeMap["LOWER_CASE"] = &lowerCaseNode;
    nodeMap["UPPER_CASE"] = &upperCaseNode;
    nodeMap["MAKE_C_IDENTIFIER"] = &makeCIdentifierNode;
    nodeMap["BOOL"] = &boolNode;
    nodeMap["ANGLE-R"] = &angle_rNode;
    nodeMap["COMMA"] = &commaNode;
    nodeMap["SEMICOLON"] = &semicolonNode;
    nodeMap["TARGET_PROPERTY"] = &targetPropertyNode;
    nodeMap["TARGET_NAME"] = &targetNameNode;
    nodeMap["TARGET_OBJECTS"] = &targetObjectsNode;
    nodeMap["TARGET_POLICY"] = &targetPolicyNode;
    nodeMap["BUILD_INTERFACE"] = &buildInterfaceNode;
    nodeMap["INSTALL_INTERFACE"] = &installInterfaceNode;
    nodeMap["INSTALL_PREFIX"] = &installPrefixNode;
    nodeMap["JOIN"] = &joinNode;
    nodeMap["LINK_ONLY"] = &linkOnlyNode;
    }
  NodeMap::const_iterator i = nodeMap.find(identifier);
  if (i == nodeMap.end())
    {
    return 0;
    }
  return i->second;

}

//----------------------------------------------------------------------------
GeneratorExpressionContent::GeneratorExpressionContent(
                                                    const char *startContent,
                                                    size_t length)
  : StartContent(startContent), ContentLength(length)
{

}

//----------------------------------------------------------------------------
std::string GeneratorExpressionContent::GetOriginalExpression() const
{
  return std::string(this->StartContent, this->ContentLength);
}

//----------------------------------------------------------------------------
std::string GeneratorExpressionContent::ProcessArbitraryContent(
    const cmGeneratorExpressionNode *node,
    const std::string &identifier,
    cmGeneratorExpressionContext *context,
    cmGeneratorExpressionDAGChecker *dagChecker,
    std::vector<std::vector<cmGeneratorExpressionEvaluator*> >::const_iterator
    pit) const
{
  std::string result;

  const
  std::vector<std::vector<cmGeneratorExpressionEvaluator*> >::const_iterator
                                      pend = this->ParamChildren.end();
  for ( ; pit != pend; ++pit)
    {
    std::vector<cmGeneratorExpressionEvaluator*>::const_iterator it
                                                            = pit->begin();
    const std::vector<cmGeneratorExpressionEvaluator*>::const_iterator end
                                                              = pit->end();
    for ( ; it != end; ++it)
      {
      if (node->RequiresLiteralInput())
        {
        if ((*it)->GetType() != cmGeneratorExpressionEvaluator::Text)
          {
          reportError(context, this->GetOriginalExpression(),
                "$<" + identifier + "> expression requires literal input.");
          return std::string();
          }
        }
      result += (*it)->Evaluate(context, dagChecker);
      if (context->HadError)
        {
        return std::string();
        }
      }
      if ((pit + 1) != pend)
        {
        result += ",";
        }
    }
  if (node->RequiresLiteralInput())
    {
    std::vector<std::string> parameters;
    parameters.push_back(result);
    return node->Evaluate(parameters, context, this, dagChecker);
    }
  return result;
}

//----------------------------------------------------------------------------
std::string GeneratorExpressionContent::Evaluate(
                            cmGeneratorExpressionContext *context,
                            cmGeneratorExpressionDAGChecker *dagChecker) const
{
  std::string identifier;
  {
  std::vector<cmGeneratorExpressionEvaluator*>::const_iterator it
                                          = this->IdentifierChildren.begin();
  const std::vector<cmGeneratorExpressionEvaluator*>::const_iterator end
                                          = this->IdentifierChildren.end();
  for ( ; it != end; ++it)
    {
    identifier += (*it)->Evaluate(context, dagChecker);
    if (context->HadError)
      {
      return std::string();
      }
    }
  }

  const cmGeneratorExpressionNode *node = GetNode(identifier);

  if (!node)
    {
    reportError(context, this->GetOriginalExpression(),
              "Expression did not evaluate to a known generator expression");
    return std::string();
    }

  if (!node->GeneratesContent())
    {
    if (node->NumExpectedParameters() == 1
        && node->AcceptsArbitraryContentParameter())
      {
      if (this->ParamChildren.empty())
        {
        reportError(context, this->GetOriginalExpression(),
                  "$<" + identifier + "> expression requires a parameter.");
        }
      }
    else
      {
      std::vector<std::string> parameters;
      this->EvaluateParameters(node, identifier, context, dagChecker,
                               parameters);
      }
    return std::string();
    }

  std::vector<std::string> parameters;
  this->EvaluateParameters(node, identifier, context, dagChecker, parameters);
  if (context->HadError)
    {
    return std::string();
    }

  return node->Evaluate(parameters, context, this, dagChecker);
}

//----------------------------------------------------------------------------
std::string GeneratorExpressionContent::EvaluateParameters(
                                const cmGeneratorExpressionNode *node,
                                const std::string &identifier,
                                cmGeneratorExpressionContext *context,
                                cmGeneratorExpressionDAGChecker *dagChecker,
                                std::vector<std::string> &parameters) const
{
  const int numExpected = node->NumExpectedParameters();
  {
  std::vector<std::vector<cmGeneratorExpressionEvaluator*> >::const_iterator
                                        pit = this->ParamChildren.begin();
  const
  std::vector<std::vector<cmGeneratorExpressionEvaluator*> >::const_iterator
                                        pend = this->ParamChildren.end();
  const bool acceptsArbitraryContent
                                  = node->AcceptsArbitraryContentParameter();
  int counter = 1;
  for ( ; pit != pend; ++pit, ++counter)
    {
    if (acceptsArbitraryContent && counter == numExpected)
      {
      std::string lastParam = this->ProcessArbitraryContent(node, identifier,
                                                            context,
                                                            dagChecker,
                                                            pit);
      parameters.push_back(lastParam);
      return std::string();
      }
    else
      {
      std::string parameter;
      std::vector<cmGeneratorExpressionEvaluator*>::const_iterator it =
                                                                pit->begin();
      const std::vector<cmGeneratorExpressionEvaluator*>::const_iterator end =
                                                                pit->end();
      for ( ; it != end; ++it)
        {
        parameter += (*it)->Evaluate(context, dagChecker);
        if (context->HadError)
          {
          return std::string();
          }
        }
      parameters.push_back(parameter);
      }
    }
  }

  if ((numExpected > cmGeneratorExpressionNode::DynamicParameters
      && (unsigned int)numExpected != parameters.size()))
    {
    if (numExpected == 0)
      {
      reportError(context, this->GetOriginalExpression(),
                  "$<" + identifier + "> expression requires no parameters.");
      }
    else if (numExpected == 1)
      {
      reportError(context, this->GetOriginalExpression(),
                  "$<" + identifier + "> expression requires "
                  "exactly one parameter.");
      }
    else
      {
      cmOStringStream e;
      e << "$<" + identifier + "> expression requires "
        << numExpected
        << " comma separated parameters, but got "
        << parameters.size() << " instead.";
      reportError(context, this->GetOriginalExpression(), e.str());
      }
    return std::string();
    }

  if (numExpected == cmGeneratorExpressionNode::OneOrMoreParameters
      && parameters.empty())
    {
    reportError(context, this->GetOriginalExpression(), "$<" + identifier
                      + "> expression requires at least one parameter.");
    }
  if (numExpected == cmGeneratorExpressionNode::OneOrZeroParameters
      && parameters.size() > 1)
    {
    reportError(context, this->GetOriginalExpression(), "$<" + identifier
                      + "> expression requires one or zero parameters.");
    }
  return std::string();
}

//----------------------------------------------------------------------------
static void deleteAll(const std::vector<cmGeneratorExpressionEvaluator*> &c)
{
  std::vector<cmGeneratorExpressionEvaluator*>::const_iterator it
                                                  = c.begin();
  const std::vector<cmGeneratorExpressionEvaluator*>::const_iterator end
                                                  = c.end();
  for ( ; it != end; ++it)
    {
    delete *it;
    }
}

//----------------------------------------------------------------------------
GeneratorExpressionContent::~GeneratorExpressionContent()
{
  deleteAll(this->IdentifierChildren);

  typedef std::vector<cmGeneratorExpressionEvaluator*> EvaluatorVector;
  std::vector<EvaluatorVector>::const_iterator pit =
                                                  this->ParamChildren.begin();
  const std::vector<EvaluatorVector>::const_iterator pend =
                                                  this->ParamChildren.end();
  for ( ; pit != pend; ++pit)
    {
    deleteAll(*pit);
    }
}
