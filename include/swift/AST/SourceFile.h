//===--- SourceFile.h - The contents of a source file -----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SOURCEFILE_H
#define SWIFT_AST_SOURCEFILE_H

#include "swift/AST/FileUnit.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/Basic/Debug.h"

namespace swift {

class PersistentParserState;

/// A file containing Swift source code.
///
/// This is a .swift or .sil file (or a virtual file, such as the contents of
/// the REPL). Since it contains raw source, it must be type checked for IR
/// generation.
class SourceFile final : public FileUnit {
  friend class ParseSourceFileRequest;

public:
  class Impl;
  struct SourceFileSyntaxInfo;

  /// Possible attributes for imports in source files.
  enum class ImportFlags {
    /// The imported module is exposed to anyone who imports the parent module.
    Exported = 0x1,

    /// This source file has access to testable declarations in the imported
    /// module.
    Testable = 0x2,

    /// This source file has access to private declarations in the imported
    /// module.
    PrivateImport = 0x4,

    /// The imported module is an implementation detail of this file and should
    /// not be required to be present if the main module is ever imported
    /// elsewhere.
    ///
    /// Mutually exclusive with Exported.
    ImplementationOnly = 0x8,

    // The module is imported to have access to named SPIs which is an
    // implementation detail of this file.
    SPIAccessControl = 0x10,

    /// Used for DenseMap.
    Reserved = 0x80
  };

  /// \see ImportFlags
  using ImportOptions = OptionSet<ImportFlags>;

  struct ImportedModuleDesc {
    ModuleDecl::ImportedModule module;
    ImportOptions importOptions;

    // Filename for a @_private import.
    StringRef filename;

    // Names of explicitly imported SPIs.
    ArrayRef<Identifier> spiGroups;

    ImportedModuleDesc(ModuleDecl::ImportedModule module, ImportOptions options,
                       StringRef filename = {},
                       ArrayRef<Identifier> spiGroups = {})
        : module(module), importOptions(options), filename(filename),
          spiGroups(spiGroups) {
      assert(!(importOptions.contains(ImportFlags::Exported) &&
               importOptions.contains(ImportFlags::ImplementationOnly)) ||
             importOptions.contains(ImportFlags::Reserved));
    }
  };

  /// Flags that direct how the source file is parsed.
  enum class ParsingFlags : uint8_t {
    /// Whether to disable delayed parsing for nominal type, extension, and
    /// function bodies.
    ///
    /// If set, type and function bodies will be parsed eagerly. Otherwise they
    /// will be lazily parsed when their contents is queried. This lets us avoid
    /// building AST nodes when they're not needed.
    ///
    /// This is set for primary files, since we want to type check all
    /// declarations and function bodies anyway, so there's no benefit in lazy
    /// parsing.
    DisableDelayedBodies = 1 << 0,

    /// Whether to disable evaluating the conditions of #if decls.
    ///
    /// If set, #if decls are parsed as-is. Otherwise, the bodies of any active
    /// clauses are hoisted such that they become sibling nodes with the #if
    /// decl.
    ///
    /// FIXME: When condition evaluation moves to a later phase, remove this
    /// and adjust the client call 'performParseOnly'.
    DisablePoundIfEvaluation = 1 << 1,

    /// Whether to suppress warnings when parsing. This is set for secondary
    /// files, as they get parsed multiple times.
    SuppressWarnings = 1 << 2
  };
  using ParsingOptions = OptionSet<ParsingFlags>;

private:
  std::unique_ptr<SourceLookupCache> Cache;
  SourceLookupCache &getCache() const;

  /// This is the list of modules that are imported by this module.
  ///
  /// This is \c None until it is filled in by the import resolution phase.
  Optional<ArrayRef<ImportedModuleDesc>> Imports;

  /// A unique identifier representing this file; used to mark private decls
  /// within the file to keep them from conflicting with other files in the
  /// same module.
  mutable Identifier PrivateDiscriminator;

  /// A synthesized file corresponding to this file, created on-demand.
  SynthesizedFileUnit *SynthesizedFile = nullptr;

  /// The root TypeRefinementContext for this SourceFile.
  ///
  /// This is set during type checking.
  TypeRefinementContext *TRC = nullptr;

  /// If non-null, used to track name lookups that happen within this file.
  Optional<ReferencedNameTracker> RequestReferencedNames;

  /// Either the class marked \@NS/UIApplicationMain or the synthesized FuncDecl
  /// that calls main on the type marked @main.
  Decl *MainDecl = nullptr;

  /// The source location of the main type.
  SourceLoc MainDeclDiagLoc;

  /// A hash of all interface-contributing tokens that have been lexed for
  /// this source file so far.
  /// We only collect interface hash for primary input files.
  llvm::Optional<llvm::MD5> InterfaceHash;

  /// The ID for the memory buffer containing this file's source.
  ///
  /// May be -1, to indicate no association with a buffer.
  int BufferID;

  /// The parsing options for the file.
  ParsingOptions ParsingOpts;

  /// The scope map that describes this source file.
  std::unique_ptr<ASTScope> Scope;

  /// The set of validated opaque return type decls in the source file.
  llvm::SmallVector<OpaqueTypeDecl *, 4> OpaqueReturnTypes;
  llvm::StringMap<OpaqueTypeDecl *> ValidatedOpaqueReturnTypes;
  /// The set of parsed decls with opaque return types that have not yet
  /// been validated.
  llvm::SetVector<ValueDecl *> UnvalidatedDeclsWithOpaqueReturnTypes;

  /// The list of top-level declarations in the source file. This is \c None if
  /// they have not yet been parsed.
  /// FIXME: Once addTopLevelDecl/prependTopLevelDecl/truncateTopLevelDecls
  /// have been removed, this can become an optional ArrayRef.
  Optional<std::vector<Decl *>> Decls;

  using SeparatelyImportedOverlayMap =
    llvm::SmallDenseMap<ModuleDecl *, llvm::SmallPtrSet<ModuleDecl *, 1>>;

  /// Keys are modules which are shadowed by one or more separately-imported
  /// overlays; values are the list of overlays shadowing them.
  ///
  /// This is used by cross-import overlays to make their members appear to
  /// be part of the underlying module. (ClangImporter overlays use a different
  /// mechanism which is not SourceFile-dependent.)
  SeparatelyImportedOverlayMap separatelyImportedOverlays;

  /// A pointer to PersistentParserState with a function reference to its
  /// deleter to handle the fact that it's forward declared.
  using ParserStatePtr =
      std::unique_ptr<PersistentParserState, void (*)(PersistentParserState *)>;

  /// Stores delayed parser state that code completion needs to be able to
  /// resume parsing at the code completion token in the file.
  ParserStatePtr DelayedParserState =
      ParserStatePtr(/*ptr*/ nullptr, /*deleter*/ nullptr);

  friend ASTContext;
  friend Impl;

public:
  /// Appends the given declaration to the end of the top-level decls list. Do
  /// not add any additional uses of this function.
  void addTopLevelDecl(Decl *d) {
    // Force decl parsing if we haven't already.
    (void)getTopLevelDecls();
    Decls->push_back(d);
  }

  /// Prepends a declaration to the top-level decls list.
  ///
  /// FIXME: This entrypoint exists to support LLDB. Calls to this function are
  /// always a mistake, and additional uses should not be added.
  ///
  /// See rdar://58355191
  void prependTopLevelDecl(Decl *d) {
    // Force decl parsing if we haven't already.
    (void)getTopLevelDecls();
    Decls->insert(Decls->begin(), d);
  }

  /// Retrieves an immutable view of the list of top-level decls in this file.
  ArrayRef<Decl *> getTopLevelDecls() const;

  /// Retrieves an immutable view of the top-level decls if they have already
  /// been parsed, or \c None if they haven't. Should only be used for dumping.
  Optional<ArrayRef<Decl *>> getCachedTopLevelDecls() const {
    if (!Decls)
      return None;
    return llvm::makeArrayRef(*Decls);
  }

  /// Truncates the list of top-level decls so it contains \c count elements. Do
  /// not add any additional uses of this function.
  void truncateTopLevelDecls(unsigned count) {
    // Force decl parsing if we haven't already.
    (void)getTopLevelDecls();
    assert(count <= Decls->size() && "Can only truncate top-level decls!");
    Decls->resize(count);
  }

  /// Retrieve the parsing options for the file.
  ParsingOptions getParsingOptions() const { return ParsingOpts; }

  /// A cache of syntax nodes that can be reused when creating the syntax tree
  /// for this file.
  swift::SyntaxParsingCache *SyntaxParsingCache = nullptr;

  /// The list of local type declarations in the source file.
  llvm::SetVector<TypeDecl *> LocalTypeDecls;

  /// A set of special declaration attributes which require the
  /// Foundation module to be imported to work. If the foundation
  /// module is still not imported by the time type checking is
  /// complete, we diagnose.
  llvm::SetVector<const DeclAttribute *> AttrsRequiringFoundation;

  /// A set of synthesized declarations that need to be type checked.
  llvm::SmallVector<Decl *, 8> SynthesizedDecls;

  /// The list of functions defined in this file whose bodies have yet to be
  /// typechecked. They must be held in this list instead of eagerly validated
  /// because their bodies may force us to perform semantic checks of arbitrary
  /// complexity, and we currently cannot handle those checks in isolation. E.g.
  /// we cannot, in general, perform witness matching on singular requirements
  /// unless the entire conformance has been evaluated.
  std::vector<AbstractFunctionDecl *> DelayedFunctions;

  /// We might perform type checking on the same source file more than once,
  /// if its the main file or a REPL instance, so keep track of the last
  /// checked synthesized declaration to avoid duplicating work.
  unsigned LastCheckedSynthesizedDecl = 0;

  /// A mapping from Objective-C selectors to the methods that have
  /// those selectors.
  llvm::DenseMap<ObjCSelector, llvm::TinyPtrVector<AbstractFunctionDecl *>>
    ObjCMethods;

  /// List of Objective-C methods, which is used for checking unintended
  /// Objective-C overrides.
  std::vector<AbstractFunctionDecl *> ObjCMethodList;

  /// An unsatisfied, optional @objc requirement in a protocol conformance.
  using ObjCUnsatisfiedOptReq = std::pair<DeclContext *, AbstractFunctionDecl *>;

  /// List of optional @objc protocol requirements that have gone
  /// unsatisfied, which might conflict with other Objective-C methods.
  std::vector<ObjCUnsatisfiedOptReq> ObjCUnsatisfiedOptReqs;

  using ObjCMethodConflict = std::tuple<ClassDecl *, ObjCSelector, bool>;

  /// List of Objective-C member conflicts we have found during type checking.
  std::vector<ObjCMethodConflict> ObjCMethodConflicts;

  /// Describes what kind of file this is, which can affect some type checking
  /// and other behavior.
  const SourceFileKind Kind;

  enum ASTStage_t {
    /// The source file has not had its imports resolved or been type checked.
    Unprocessed,
    /// Import resolution has completed.
    ImportsResolved,
    /// Type checking has completed.
    TypeChecked
  };

  /// Defines what phases of parsing and semantic analysis are complete for a
  /// source file.
  ///
  /// Only files that have been fully processed (i.e. type-checked) will be
  /// forwarded on to IRGen.
  ASTStage_t ASTStage = Unprocessed;

  /// Virtual file paths declared by \c #sourceLocation(file:) declarations in
  /// this source file.
  llvm::SmallVector<Located<StringRef>, 0> VirtualFilePaths;

  /// Returns information about the file paths used for diagnostics and magic
  /// identifiers in this source file, including virtual filenames introduced by
  /// \c #sourceLocation(file:) declarations.
  llvm::StringMap<SourceFilePathInfo> getInfoForUsedFilePaths() const;

  SourceFile(ModuleDecl &M, SourceFileKind K, Optional<unsigned> bufferID,
             bool KeepParsedTokens = false, bool KeepSyntaxTree = false,
             ParsingOptions parsingOpts = {});

  ~SourceFile();

  /// Retrieve an immutable view of the source file's imports.
  ArrayRef<ImportedModuleDesc> getImports() const { return *Imports; }

  /// Set the imports for this source file. This gets called by import
  /// resolution.
  void setImports(ArrayRef<ImportedModuleDesc> imports);

  enum ImportQueryKind {
    /// Return the results for testable or private imports.
    TestableAndPrivate,
    /// Return the results only for testable imports.
    TestableOnly,
    /// Return the results only for private imports.
    PrivateOnly
  };

  bool
  hasTestableOrPrivateImport(AccessLevel accessLevel, const ValueDecl *ofDecl,
                             ImportQueryKind kind = TestableAndPrivate) const;

  /// Does this source file have any implementation-only imports?
  /// If not, we can fast-path module checks.
  bool hasImplementationOnlyImports() const;

  bool isImportedImplementationOnly(const ModuleDecl *module) const;

  /// Find all SPI names imported from \p importedModule by this file,
  /// collecting the identifiers in \p spiGroups.
  virtual void
  lookupImportedSPIGroups(const ModuleDecl *importedModule,
                         SmallVectorImpl<Identifier> &spiGroups) const override;

  // Is \p targetDecl accessible as an explictly imported SPI from this file?
  bool isImportedAsSPI(const ValueDecl *targetDecl) const;

  bool shouldCrossImport() const;

  /// Register a separately-imported overlay as shadowing the module that
  /// declares it.
  ///
  /// \returns true if the overlay was added; false if it already existed.
  bool addSeparatelyImportedOverlay(ModuleDecl *overlay,
                                    ModuleDecl *declaring) {
    return std::get<1>(separatelyImportedOverlays[declaring].insert(overlay));
  }

  /// Retrieves a list of separately imported overlays which are shadowing
  /// \p declaring. If any \p overlays are returned, qualified lookups into
  /// \p declaring should be performed into \p overlays instead; since they
  /// are overlays, they will re-export \p declaring, but will also augment it
  /// with additional symbols.
  void getSeparatelyImportedOverlays(
      ModuleDecl *declaring, SmallVectorImpl<ModuleDecl *> &overlays) {
    auto i = separatelyImportedOverlays.find(declaring);
    if (i == separatelyImportedOverlays.end()) return;

    auto &value = std::get<1>(*i);
    overlays.append(value.begin(), value.end());
  }

  SWIFT_DEBUG_DUMPER(dumpSeparatelyImportedOverlays());

  void cacheVisibleDecls(SmallVectorImpl<ValueDecl *> &&globals) const;
  const SmallVectorImpl<ValueDecl *> &getCachedVisibleDecls() const;

  virtual void lookupValue(DeclName name, NLKind lookupKind,
                           SmallVectorImpl<ValueDecl*> &result) const override;

  virtual void lookupVisibleDecls(ModuleDecl::AccessPathTy accessPath,
                                  VisibleDeclConsumer &consumer,
                                  NLKind lookupKind) const override;

  virtual void lookupClassMembers(ModuleDecl::AccessPathTy accessPath,
                                  VisibleDeclConsumer &consumer) const override;
  virtual void
  lookupClassMember(ModuleDecl::AccessPathTy accessPath, DeclName name,
                    SmallVectorImpl<ValueDecl*> &results) const override;

  void lookupObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const override;

protected:
  virtual void
  lookupOperatorDirect(Identifier name, OperatorFixity fixity,
                       TinyPtrVector<OperatorDecl *> &results) const override;

  virtual void lookupPrecedenceGroupDirect(
      Identifier name,
      TinyPtrVector<PrecedenceGroupDecl *> &results) const override;

public:
  virtual void getTopLevelDecls(SmallVectorImpl<Decl*> &results) const override;

  virtual void
  getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results) const override;

  virtual void
  getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl*> &results) const override;

  virtual TypeDecl *lookupLocalType(llvm::StringRef MangledName) const override;

  virtual void
  getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &results) const override;
  virtual void
  getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &results) const override;

  virtual void
  getImportedModules(SmallVectorImpl<ModuleDecl::ImportedModule> &imports,
                     ModuleDecl::ImportFilter filter) const override;

  virtual void
  collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const override;

  Identifier getDiscriminatorForPrivateValue(const ValueDecl *D) const override;
  Identifier getPrivateDiscriminator() const { return PrivateDiscriminator; }
  Optional<BasicDeclLocs> getBasicLocsForDecl(const Decl *D) const override;

  /// Returns the synthesized file for this source file, if it exists.
  SynthesizedFileUnit *getSynthesizedFile() const { return SynthesizedFile; };

  SynthesizedFileUnit &getOrCreateSynthesizedFile();

  virtual bool walk(ASTWalker &walker) override;

  ReferencedNameTracker *getRequestBasedReferencedNameTracker() {
    return RequestReferencedNames ? RequestReferencedNames.getPointer() : nullptr;
  }
  const ReferencedNameTracker *getRequestBasedReferencedNameTracker() const {
    return RequestReferencedNames ? RequestReferencedNames.getPointer() : nullptr;
  }

  /// Creates and installs the referenced name trackers in this source file.
  ///
  /// This entrypoint must be called before incremental compilation can proceed,
  /// else reference dependencies will not be registered.
  void createReferencedNameTracker();

  /// Retrieves the appropriate referenced name tracker instance.
  ///
  /// If incremental dependencies tracking is not enabled or \c createReferencedNameTracker()
  /// has not been invoked on this source file, the result is \c nullptr.
  const ReferencedNameTracker *getConfiguredReferencedNameTracker() const;

  /// The buffer ID for the file that was imported, or None if there
  /// is no associated buffer.
  Optional<unsigned> getBufferID() const {
    if (BufferID == -1)
      return None;
    return BufferID;
  }

  /// If this buffer corresponds to a file on disk, returns the path.
  /// Otherwise, return an empty string.
  StringRef getFilename() const;

  /// Retrieve the scope that describes this source file.
  ASTScope &getScope();

  /// Retrieves the previously set delayed parser state, asserting that it
  /// exists.
  PersistentParserState *getDelayedParserState() {
    // Force parsing of the top-level decls, which will set DelayedParserState
    // if necessary.
    // FIXME: Ideally the parser state should be an output of
    // ParseSourceFileRequest, but the evaluator doesn't currently support
    // move-only outputs for cached requests.
    (void)getTopLevelDecls();

    auto *state = DelayedParserState.get();
    assert(state && "Didn't set any delayed parser state!");
    return state;
  }

  /// Record delayed parser state for the source file. This is needed for code
  /// completion's second pass.
  void setDelayedParserState(ParserStatePtr &&state) {
    DelayedParserState = std::move(state);
  }

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os) const;

  /// Pretty-print the contents of this source file.
  ///
  /// \param Printer The AST printer used for printing the contents.
  /// \param PO Options controlling the printing process.
  void print(ASTPrinter &Printer, const PrintOptions &PO);
  void print(raw_ostream &OS, const PrintOptions &PO);

  static bool classof(const FileUnit *file) {
    return file->getKind() == FileUnitKind::Source;
  }
  static bool classof(const DeclContext *DC) {
    return isa<FileUnit>(DC) && classof(cast<FileUnit>(DC));
  }

  /// True if this is a "script mode" source file that admits top-level code.
  bool isScriptMode() const {
    switch (Kind) {
    case SourceFileKind::Main:
    case SourceFileKind::REPL:
      return true;

    case SourceFileKind::Library:
    case SourceFileKind::Interface:
    case SourceFileKind::SIL:
      return false;
    }
    llvm_unreachable("bad SourceFileKind");
  }

  Decl *getMainDecl() const override { return MainDecl; }
  SourceLoc getMainDeclDiagLoc() const {
    assert(hasMainDecl());
    return MainDeclDiagLoc;
  }
  SourceLoc getMainClassDiagLoc() const {
    assert(hasMainClass());
    return getMainDeclDiagLoc();
  }

  /// Register a "main" class for the module, complaining if there is more than
  /// one.
  ///
  /// Should only be called during type-checking.
  bool registerMainDecl(Decl *mainDecl, SourceLoc diagLoc);

  /// True if this source file has an application entry point.
  ///
  /// This is true if the source file either is in script mode or contains
  /// a designated main class.
  bool hasEntryPoint() const override {
    return isScriptMode() || hasMainDecl();
  }

  /// Get the root refinement context for the file. The root context may be
  /// null if the context hierarchy has not been built yet. Use
  /// TypeChecker::getOrBuildTypeRefinementContext() to get a built
  /// root of the hierarchy.
  TypeRefinementContext *getTypeRefinementContext();

  /// Set the root refinement context for the file.
  void setTypeRefinementContext(TypeRefinementContext *TRC);

  void enableInterfaceHash() {
    assert(!hasInterfaceHash());
    InterfaceHash.emplace();
  }

  bool hasInterfaceHash() const {
    return InterfaceHash.hasValue();
  }

  NullablePtr<llvm::MD5> getInterfaceHashPtr() {
    return InterfaceHash ? InterfaceHash.getPointer() : nullptr;
  }

  void getInterfaceHash(llvm::SmallString<32> &str) const {
    // Copy to preserve idempotence.
    llvm::MD5 md5 = *InterfaceHash;
    llvm::MD5::MD5Result result;
    md5.final(result);
    llvm::MD5::stringifyResult(result, str);
  }

  void dumpInterfaceHash(llvm::raw_ostream &out) {
    llvm::SmallString<32> str;
    getInterfaceHash(str);
    out << str << '\n';
  }

  std::vector<Token> &getTokenVector();

  ArrayRef<Token> getAllTokens() const;

  bool shouldCollectToken() const;

  bool shouldBuildSyntaxTree() const;

  bool canBeParsedInFull() const;

  bool isSuitableForASTScopes() const { return canBeParsedInFull(); }

  /// Whether the bodies of types and functions within this file can be lazily
  /// parsed.
  bool hasDelayedBodyParsing() const;

  syntax::SourceFileSyntax getSyntaxRoot() const;
  void setSyntaxRoot(syntax::SourceFileSyntax &&Root);
  bool hasSyntaxRoot() const;

  OpaqueTypeDecl *lookupOpaqueResultType(StringRef MangledName) override;

  /// Do not call when inside an inactive clause (\c
  /// InInactiveClauseEnvironment)) because it will later on result in a lookup
  /// to something that won't be in the ASTScope tree.
  void addUnvalidatedDeclWithOpaqueResultType(ValueDecl *vd) {
    UnvalidatedDeclsWithOpaqueReturnTypes.insert(vd);
  }

  ArrayRef<OpaqueTypeDecl *> getOpaqueReturnTypeDecls();

private:

  /// If not None, the underlying vector should contain tokens of this source file.
  Optional<std::vector<Token>> AllCorrectedTokens;

  std::unique_ptr<SourceFileSyntaxInfo> SyntaxInfo;
};

inline SourceFile::ParsingOptions operator|(SourceFile::ParsingFlags lhs,
                                            SourceFile::ParsingFlags rhs) {
  return SourceFile::ParsingOptions(lhs) | rhs;
}

inline SourceFile &
ModuleDecl::getMainSourceFile(SourceFileKind expectedKind) const {
  assert(!Files.empty() && "No files added yet");
  assert(cast<SourceFile>(Files.front())->Kind == expectedKind);
  return *cast<SourceFile>(Files.front());
}

inline FileUnit *ModuleDecl::EntryPointInfoTy::getEntryPointFile() const {
  return storage.getPointer();
}
inline void ModuleDecl::EntryPointInfoTy::setEntryPointFile(FileUnit *file) {
  assert(!storage.getPointer());
  storage.setPointer(file);
}

inline bool ModuleDecl::EntryPointInfoTy::hasEntryPoint() const {
  return storage.getPointer();
}

inline bool ModuleDecl::EntryPointInfoTy::markDiagnosedMultipleMainClasses() {
  bool res = storage.getInt().contains(Flags::DiagnosedMultipleMainClasses);
  storage.setInt(storage.getInt() | Flags::DiagnosedMultipleMainClasses);
  return !res;
}

inline bool ModuleDecl::EntryPointInfoTy::markDiagnosedMainClassWithScript() {
  bool res = storage.getInt().contains(Flags::DiagnosedMainClassWithScript);
  storage.setInt(storage.getInt() | Flags::DiagnosedMainClassWithScript);
  return !res;
}

inline void simple_display(llvm::raw_ostream &out, const SourceFile *SF) {
  assert(SF && "Cannot display null source file!");

  out << "source_file " << '\"' << SF->getFilename() << '\"';
}
} // end namespace swift

namespace llvm {

template<>
struct DenseMapInfo<swift::SourceFile::ImportOptions> {
  using ImportOptions = swift::SourceFile::ImportOptions;

  using UnsignedDMI = DenseMapInfo<uint8_t>;

  static inline ImportOptions getEmptyKey() {
    return ImportOptions(UnsignedDMI::getEmptyKey());
  }
  static inline ImportOptions getTombstoneKey() {
    return ImportOptions(UnsignedDMI::getTombstoneKey());
  }
  static inline unsigned getHashValue(ImportOptions options) {
    return UnsignedDMI::getHashValue(options.toRaw());
  }
  static bool isEqual(ImportOptions a, ImportOptions b) {
    return UnsignedDMI::isEqual(a.toRaw(), b.toRaw());
  }
};

template<>
struct DenseMapInfo<swift::SourceFile::ImportedModuleDesc> {
  using ImportedModuleDesc = swift::SourceFile::ImportedModuleDesc;

  using ImportedModuleDMI = DenseMapInfo<swift::ModuleDecl::ImportedModule>;
  using ImportOptionsDMI = DenseMapInfo<swift::SourceFile::ImportOptions>;
  using StringRefDMI = DenseMapInfo<StringRef>;

  static inline ImportedModuleDesc getEmptyKey() {
    return ImportedModuleDesc(ImportedModuleDMI::getEmptyKey(),
                              ImportOptionsDMI::getEmptyKey(),
                              StringRefDMI::getEmptyKey());
  }
  static inline ImportedModuleDesc getTombstoneKey() {
    return ImportedModuleDesc(ImportedModuleDMI::getTombstoneKey(),
                              ImportOptionsDMI::getTombstoneKey(),
                              StringRefDMI::getTombstoneKey());
  }
  static inline unsigned getHashValue(const ImportedModuleDesc &import) {
    return combineHashValue(ImportedModuleDMI::getHashValue(import.module),
           combineHashValue(ImportOptionsDMI::getHashValue(import.importOptions),
                            StringRefDMI::getHashValue(import.filename)));
  }
  static bool isEqual(const ImportedModuleDesc &a,
                      const ImportedModuleDesc &b) {
    return ImportedModuleDMI::isEqual(a.module, b.module) &&
           ImportOptionsDMI::isEqual(a.importOptions, b.importOptions) &&
           StringRefDMI::isEqual(a.filename, b.filename);
  }
};
}

#endif
