//===--- Dex.cpp - Dex Symbol Index Implementation --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Dex.h"
#include "FileDistance.h"
#include "FuzzyMatch.h"
#include "Logger.h"
#include "Quality.h"
#include "Trace.h"
#include "index/Index.h"
#include "index/dex/Iterator.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <queue>

namespace clang {
namespace clangd {
namespace dex {

namespace {

// Mark symbols which are can be used for code completion.
const Token RestrictedForCodeCompletion =
    Token(Token::Kind::Sentinel, "Restricted For Code Completion");

// Returns the tokens which are given symbol's characteristics. Currently, the
// generated tokens only contain fuzzy matching trigrams and symbol's scope,
// but in the future this will also return path proximity tokens and other
// types of tokens such as symbol type (if applicable).
// Returns the tokens which are given symbols's characteristics. For example,
// trigrams and scopes.
// FIXME(kbobyrev): Support more token types:
// * Types
// * Namespace proximity
std::vector<Token> generateSearchTokens(const Symbol &Sym) {
  std::vector<Token> Result = generateIdentifierTrigrams(Sym.Name);
  Result.emplace_back(Token::Kind::Scope, Sym.Scope);
  // Skip token generation for symbols with unknown declaration location.
  if (!Sym.CanonicalDeclaration.FileURI.empty())
    for (const auto &ProximityURI :
         generateProximityURIs(Sym.CanonicalDeclaration.FileURI))
      Result.emplace_back(Token::Kind::ProximityURI, ProximityURI);
  if (Sym.Flags & Symbol::IndexedForCodeCompletion)
    Result.emplace_back(RestrictedForCodeCompletion);
  return Result;
}

// Constructs BOOST iterators for Path Proximities.
std::vector<std::unique_ptr<Iterator>> createFileProximityIterators(
    llvm::ArrayRef<std::string> ProximityPaths,
    llvm::ArrayRef<std::string> URISchemes,
    const llvm::DenseMap<Token, PostingList> &InvertedIndex) {
  std::vector<std::unique_ptr<Iterator>> BoostingIterators;
  // Deduplicate parent URIs extracted from the ProximityPaths.
  llvm::StringSet<> ParentURIs;
  llvm::StringMap<SourceParams> Sources;
  for (const auto &Path : ProximityPaths) {
    Sources[Path] = SourceParams();
    auto PathURI = URI::create(Path, URISchemes);
    if (!PathURI) {
      elog("Given ProximityPath {0} is can not be converted to any known URI "
           "scheme. fuzzyFind request will ignore it.",
           Path);
      llvm::consumeError(PathURI.takeError());
      continue;
    }
    const auto PathProximityURIs = generateProximityURIs(PathURI->toString());
    for (const auto &ProximityURI : PathProximityURIs)
      ParentURIs.insert(ProximityURI);
  }
  // Use SymbolRelevanceSignals for symbol relevance evaluation: use defaults
  // for all parameters except for Proximity Path distance signal.
  SymbolRelevanceSignals PathProximitySignals;
  // DistanceCalculator will find the shortest distance from ProximityPaths to
  // any URI extracted from the ProximityPaths.
  URIDistance DistanceCalculator(Sources);
  PathProximitySignals.FileProximityMatch = &DistanceCalculator;
  // Try to build BOOST iterator for each Proximity Path provided by
  // ProximityPaths. Boosting factor should depend on the distance to the
  // Proximity Path: the closer processed path is, the higher boosting factor.
  for (const auto &ParentURI : ParentURIs.keys()) {
    const auto It =
        InvertedIndex.find(Token(Token::Kind::ProximityURI, ParentURI));
    if (It != InvertedIndex.end()) {
      // FIXME(kbobyrev): Append LIMIT on top of every BOOST iterator.
      PathProximitySignals.SymbolURI = ParentURI;
      BoostingIterators.push_back(
          createBoost(It->second.iterator(), PathProximitySignals.evaluate()));
    }
  }
  return BoostingIterators;
}

} // namespace

void Dex::buildIndex() {
  std::vector<std::pair<float, const Symbol *>> ScoredSymbols(Symbols.size());

  for (size_t I = 0; I < Symbols.size(); ++I) {
    const Symbol *Sym = Symbols[I];
    LookupTable[Sym->ID] = Sym;
    ScoredSymbols[I] = {quality(*Sym), Sym};
  }

  // Symbols are sorted by symbol qualities so that items in the posting lists
  // are stored in the descending order of symbol quality.
  std::sort(begin(ScoredSymbols), end(ScoredSymbols),
            std::greater<std::pair<float, const Symbol *>>());

  // SymbolQuality was empty up until now.
  SymbolQuality.resize(Symbols.size());
  // Populate internal storage using Symbol + Score pairs.
  for (size_t I = 0; I < ScoredSymbols.size(); ++I) {
    SymbolQuality[I] = ScoredSymbols[I].first;
    Symbols[I] = ScoredSymbols[I].second;
  }

  // Populate TempInvertedIndex with lists for index symbols.
  llvm::DenseMap<Token, std::vector<DocID>> TempInvertedIndex;
  for (DocID SymbolRank = 0; SymbolRank < Symbols.size(); ++SymbolRank) {
    const auto *Sym = Symbols[SymbolRank];
    for (const auto &Token : generateSearchTokens(*Sym))
      TempInvertedIndex[Token].push_back(SymbolRank);
  }

  // Convert lists of items to posting lists.
  for (const auto &TokenToPostingList : TempInvertedIndex)
    InvertedIndex.insert(
        {TokenToPostingList.first, PostingList(TokenToPostingList.second)});
}

/// Constructs iterators over tokens extracted from the query and exhausts it
/// while applying Callback to each symbol in the order of decreasing quality
/// of the matched symbols.
bool Dex::fuzzyFind(const FuzzyFindRequest &Req,
                    llvm::function_ref<void(const Symbol &)> Callback) const {
  assert(!StringRef(Req.Query).contains("::") &&
         "There must be no :: in query.");
  // FIXME: attach the query tree to the trace span.
  trace::Span Tracer("Dex fuzzyFind");
  FuzzyMatcher Filter(Req.Query);
  bool More = false;

  std::vector<std::unique_ptr<Iterator>> TopLevelChildren;
  const auto TrigramTokens = generateQueryTrigrams(Req.Query);

  // Generate query trigrams and construct AND iterator over all query
  // trigrams.
  std::vector<std::unique_ptr<Iterator>> TrigramIterators;
  for (const auto &Trigram : TrigramTokens) {
    const auto It = InvertedIndex.find(Trigram);
    if (It != InvertedIndex.end())
      TrigramIterators.push_back(It->second.iterator());
  }
  if (!TrigramIterators.empty())
    TopLevelChildren.push_back(createAnd(move(TrigramIterators)));

  // Generate scope tokens for search query.
  std::vector<std::unique_ptr<Iterator>> ScopeIterators;
  for (const auto &Scope : Req.Scopes) {
    const auto It = InvertedIndex.find(Token(Token::Kind::Scope, Scope));
    if (It != InvertedIndex.end())
      ScopeIterators.push_back(It->second.iterator());
  }
  if (Req.AnyScope)
    ScopeIterators.push_back(createBoost(createTrue(Symbols.size()),
                                         ScopeIterators.empty() ? 1.0 : 0.2));

  // Add OR iterator for scopes if there are any Scope Iterators.
  if (!ScopeIterators.empty())
    TopLevelChildren.push_back(createOr(move(ScopeIterators)));

  // Add proximity paths boosting.
  auto BoostingIterators = createFileProximityIterators(
      Req.ProximityPaths, URISchemes, InvertedIndex);
  // Boosting iterators do not actually filter symbols. In order to preserve
  // the validity of resulting query, TRUE iterator should be added along
  // BOOSTs.
  if (!BoostingIterators.empty()) {
    BoostingIterators.push_back(createTrue(Symbols.size()));
    TopLevelChildren.push_back(createOr(move(BoostingIterators)));
  }

  if (Req.RestrictForCodeCompletion)
    TopLevelChildren.push_back(
        InvertedIndex.find(RestrictedForCodeCompletion)->second.iterator());

  // Use TRUE iterator if both trigrams and scopes from the query are not
  // present in the symbol index.
  auto QueryIterator = TopLevelChildren.empty()
                           ? createTrue(Symbols.size())
                           : createAnd(move(TopLevelChildren));
  // Retrieve more items than it was requested: some of  the items with high
  // final score might not be retrieved otherwise.
  // FIXME(kbobyrev): Pre-scoring retrieval threshold should be adjusted as
  // using 100x of the requested number might not be good in practice, e.g.
  // when the requested number of items is small.
  auto Root = Req.Limit ? createLimit(move(QueryIterator), *Req.Limit * 100)
                        : move(QueryIterator);

  using IDAndScore = std::pair<DocID, float>;
  std::vector<IDAndScore> IDAndScores = consume(*Root);

  auto Compare = [](const IDAndScore &LHS, const IDAndScore &RHS) {
    return LHS.second > RHS.second;
  };
  TopN<IDAndScore, decltype(Compare)> Top(
      Req.Limit ? *Req.Limit : std::numeric_limits<size_t>::max(), Compare);
  for (const auto &IDAndScore : IDAndScores) {
    const DocID SymbolDocID = IDAndScore.first;
    const auto *Sym = Symbols[SymbolDocID];
    const llvm::Optional<float> Score = Filter.match(Sym->Name);
    if (!Score)
      continue;
    // Combine Fuzzy Matching score, precomputed symbol quality and boosting
    // score for a cumulative final symbol score.
    const float FinalScore =
        (*Score) * SymbolQuality[SymbolDocID] * IDAndScore.second;
    // If Top.push(...) returns true, it means that it had to pop an item. In
    // this case, it is possible to retrieve more symbols.
    if (Top.push({SymbolDocID, FinalScore}))
      More = true;
  }

  // Apply callback to the top Req.Limit items in the descending
  // order of cumulative score.
  for (const auto &Item : std::move(Top).items())
    Callback(*Symbols[Item.first]);
  return More;
}

void Dex::lookup(const LookupRequest &Req,
                 llvm::function_ref<void(const Symbol &)> Callback) const {
  trace::Span Tracer("Dex lookup");
  for (const auto &ID : Req.IDs) {
    auto I = LookupTable.find(ID);
    if (I != LookupTable.end())
      Callback(*I->second);
  }
}

void Dex::refs(const RefsRequest &Req,
               llvm::function_ref<void(const Ref &)> Callback) const {
  trace::Span Tracer("Dex refs");
  log("refs is not implemented.");
}

size_t Dex::estimateMemoryUsage() const {
  size_t Bytes = Symbols.size() * sizeof(const Symbol *);
  Bytes += SymbolQuality.size() * sizeof(float);
  Bytes += LookupTable.getMemorySize();
  Bytes += InvertedIndex.getMemorySize();
  for (const auto &TokenToPostingList : InvertedIndex)
    Bytes += TokenToPostingList.second.bytes();
  return Bytes + BackingDataSize;
}

std::vector<std::string> generateProximityURIs(llvm::StringRef URIPath) {
  std::vector<std::string> Result;
  auto ParsedURI = URI::parse(URIPath);
  assert(ParsedURI &&
         "Non-empty argument of generateProximityURIs() should be a valid "
         "URI.");
  StringRef Body = ParsedURI->body();
  // FIXME(kbobyrev): Currently, this is a heuristic which defines the maximum
  // size of resulting vector. Some projects might want to have higher limit if
  // the file hierarchy is deeper. For the generic case, it would be useful to
  // calculate Limit in the index build stage by calculating the maximum depth
  // of the project source tree at runtime.
  size_t Limit = 5;
  // Insert original URI before the loop: this would save a redundant iteration
  // with a URI parse.
  Result.emplace_back(ParsedURI->toString());
  while (!Body.empty() && --Limit > 0) {
    // FIXME(kbobyrev): Parsing and encoding path to URIs is not necessary and
    // could be optimized.
    Body = llvm::sys::path::parent_path(Body, llvm::sys::path::Style::posix);
    URI TokenURI(ParsedURI->scheme(), ParsedURI->authority(), Body);
    if (!Body.empty())
      Result.emplace_back(TokenURI.toString());
  }
  return Result;
}

} // namespace dex
} // namespace clangd
} // namespace clang
