//===--- swift-reflection-dump.cpp - Reflection testing application -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// This is a host-side tool to dump remote reflection sections in swift
// binaries.
//===----------------------------------------------------------------------===//

#include "swift/ABI/MetadataValues.h"
#include "swift/Basic/Demangle.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Reflection/TypeRef.h"
#include "swift/Reflection/TypeRefBuilder.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/CommandLine.h"

#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <csignal>

using llvm::dyn_cast;
using llvm::StringRef;
using llvm::ArrayRef;
using namespace llvm::object;

using namespace swift;
using namespace swift::reflection;
using namespace swift::remote;
using namespace Demangle;

enum class ActionType {
  None,
  DumpReflectionSections,
  DumpHeapInstance
};

namespace options {
static llvm::cl::opt<ActionType>
Action(llvm::cl::desc("Mode:"),
       llvm::cl::values(
         clEnumValN(ActionType::DumpReflectionSections,
                    "dump-reflection-sections",
                    "Dump the field reflection section"),
         clEnumValN(ActionType::DumpHeapInstance,
                    "dump-heap-instance",
                    "Dump the field layout for a heap instance by running "
                    "a Swift executable"),
         clEnumValEnd));

static llvm::cl::opt<std::string>
BinaryFilename("binary-filename", llvm::cl::desc("Filename of the binary file"),
               llvm::cl::Required);

static llvm::cl::opt<std::string>
Architecture("arch", llvm::cl::desc("Architecture to inspect in the binary"),
             llvm::cl::Required);
} // end namespace options

template<typename T>
static T unwrap(llvm::ErrorOr<T> value) {
  if (!value.getError())
    return std::move(value.get());
  std::cerr << "swift-reflection-test error: " << value.getError().message() << "\n";
  exit(EXIT_FAILURE);
}

static llvm::object::SectionRef
getSectionRef(const ObjectFile *objectFile,
              ArrayRef<StringRef> anySectionNames) {
  for (auto section : objectFile->sections()) {
    StringRef sectionName;
    section.getName(sectionName);
    for (auto desiredName : anySectionNames) {
      if (sectionName.equals(desiredName)) {
        return section;
      }
    }
  }
  return SectionRef();
}

static int doDumpReflectionSections(std::string binaryFilename,
                                    StringRef arch) {
  // Note: binaryOrError and objectOrError own the memory for our ObjectFile;
  // once they go out of scope, we can no longer do anything.
  OwningBinary<Binary> binaryOwner;
  std::unique_ptr<llvm::object::ObjectFile> objectOwner;

  binaryOwner = unwrap(llvm::object::createBinary(binaryFilename));
  const llvm::object::Binary *binaryFile = binaryOwner.getBinary();

  // The object file we are doing lookups in -- either the binary itself, or
  // a particular slice of a universal binary.
  const ObjectFile *objectFile;

  if (auto o = dyn_cast<ObjectFile>(binaryFile)) {
    objectFile = o;
  } else {
    auto universal = cast<MachOUniversalBinary>(binaryFile);
    objectOwner = unwrap(universal->getObjectForArch(arch));
    objectFile = objectOwner.get();
  }

  // Field descriptor section
  auto fieldSectionRef = getSectionRef(objectFile, {
    "__swift3_fieldmd", ".swift3_fieldmd"
  });

  if (fieldSectionRef.getObject() == nullptr) {
    std::cerr << binaryFilename;
    std::cerr << " doesn't have a field reflection section!\n";
    return EXIT_FAILURE;
  }

  StringRef fieldSectionContents;
  fieldSectionRef.getContents(fieldSectionContents);

  const FieldSection fieldSection {
    reinterpret_cast<const void *>(fieldSectionContents.begin()),
    reinterpret_cast<const void *>(fieldSectionContents.end())
  };

  // Associated type section - optional
  AssociatedTypeSection associatedTypeSection {nullptr, nullptr};

  auto associatedTypeSectionRef = getSectionRef(objectFile, {
    "__swift3_assocty", ".swift3_assocty"
  });

  if (associatedTypeSectionRef.getObject() != nullptr) {
    StringRef associatedTypeSectionContents;
    associatedTypeSectionRef.getContents(associatedTypeSectionContents);
    associatedTypeSection = {
      reinterpret_cast<const void *>(associatedTypeSectionContents.begin()),
      reinterpret_cast<const void *>(associatedTypeSectionContents.end()),
    };
  }

  // Builtin types section
  BuiltinTypeSection builtinTypeSection {nullptr, nullptr};

  auto builtinTypeSectionRef = getSectionRef(objectFile, {
    "__swift3_builtin", ".swift3_builtin"
  });

  if (builtinTypeSectionRef.getObject() != nullptr) {
    StringRef builtinTypeSectionContents;
    builtinTypeSectionRef.getContents(builtinTypeSectionContents);

    builtinTypeSection = {
      reinterpret_cast<const void *>(builtinTypeSectionContents.begin()),
      reinterpret_cast<const void *>(builtinTypeSectionContents.end())
    };
  }

  // Typeref section
  auto typeRefSectionRef = getSectionRef(objectFile, {
    "__swift3_typeref", ".swift3_typeref"
  });

  if (typeRefSectionRef.getObject() == nullptr) {
    std::cerr << binaryFilename;
    std::cerr << " doesn't have an associated typeref section!\n";
    return EXIT_FAILURE;
  }

  StringRef typeRefSectionContents;
  typeRefSectionRef.getContents(typeRefSectionContents);

  const GenericSection typeRefSection {
    reinterpret_cast<const void *>(typeRefSectionContents.begin()),
    reinterpret_cast<const void *>(typeRefSectionContents.end())
  };

  // Reflection strings section
  auto reflectionStringsSectionRef = getSectionRef(objectFile, {
    "__swift3_reflstr", ".swift3_reflstr"
  });

  if (reflectionStringsSectionRef.getObject() == nullptr) {
    std::cerr << binaryFilename;
    std::cerr << " doesn't have an associated reflection strings section!\n";
    return EXIT_FAILURE;
  }

  StringRef reflectionStringsSectionContents;
  reflectionStringsSectionRef.getContents(reflectionStringsSectionContents);

  const GenericSection reflectionStringsSection {
    reinterpret_cast<const void *>(reflectionStringsSectionContents.begin()),
    reinterpret_cast<const void *>(reflectionStringsSectionContents.end())
  };

  // Construct the TypeRefBuilder
  TypeRefBuilder builder;
  builder.addReflectionInfo({
    binaryFilename,
    fieldSection,
    associatedTypeSection,
    builtinTypeSection,
    typeRefSection,
    reflectionStringsSection,
  });

  // Dump everything
  builder.dumpAllSections(std::cout);

  return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift Reflection Dump\n");
  return doDumpReflectionSections(options::BinaryFilename,
                                  options::Architecture);
}

