#ifndef _src_stack_stack_h
#define _src_stack_stack_h

#include "../shared/extern.h"

#ifdef _WIN32
#include <windows.h>
#define IMAGE_DATA_DIRECTORY_DEFINED
#endif

#include "cor.h"
#include "corprof.h"
#include <string>
#include <vector>

struct ArgumentInfo {
  std::wstring name;
  std::wstring type;
};

struct FunctionInfo {
  FunctionID functionId = 0;
  ModuleID moduleId = 0;
  ClassID classId = 0;
  mdMethodDef token = 0;
  bool isStatic = false;

  std::wstring className;
  std::wstring classDefinition;
  std::wstring methodName;
  std::vector<std::wstring> methodTypeParams;
  std::vector<std::wstring> methodTypeArgs;
  std::wstring returnType;
  std::vector<ArgumentInfo> args;
};

std::wstring GetClassName(ICorProfilerInfo2 *info, ClassID classId);

std::wstring GetClassDefinitionName(ICorProfilerInfo2 *info, ClassID classId);

std::wstring GetTypeDefinitionName(IMetaDataImport2 *import, mdTypeDef typeDef);

std::wstring GetTypeSimpleName(IMetaDataImport2 *import, mdTypeDef typeDef);

HRESULT GetFunctionInfo(ICorProfilerInfo2 *info, FunctionID functionId,
                        FunctionInfo &out, COR_PRF_FRAME_INFO frameInfo = 0);

std::wstring GetFunctionName(const FunctionInfo &fn);

std::wstring GetFunctionFullName(const FunctionInfo &fn);

std::wstring GetFunctionDefinition(const FunctionInfo &fn);

std::vector<ArgumentInfo> GetFunctionArgs(const FunctionInfo &fn);

std::wstring GetFunctionArgsString(const FunctionInfo &fn);

#endif
