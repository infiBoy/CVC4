/*********************                                                        */
/*! \file theory_proof.cpp
 ** \verbatim
 ** Original author: Liana Hadarean
 ** Major contributors: Morgan Deters
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief [[ Add one-line brief description here ]]
 **
 ** [[ Add lengthier description here ]]
 ** \todo document this file
 **/
#include "proof/theory_proof.h"

#include "base/cvc4_assert.h"
#include "context/context.h"
#include "options/bv_options.h"
#include "options/proof_options.h"
#include "proof/arith_proof.h"
#include "proof/array_proof.h"
#include "proof/bitvector_proof.h"
#include "proof/clause_id.h"
#include "proof/cnf_proof.h"
#include "proof/proof_manager.h"
#include "proof/proof_output_channel.h"
#include "proof/proof_utils.h"
#include "proof/sat_proof.h"
#include "proof/uf_proof.h"
#include "prop/sat_solver_types.h"
#include "smt/smt_engine.h"
#include "smt/smt_engine_scope.h"
#include "smt_util/node_visitor.h"
#include "theory/arrays/theory_arrays.h"
#include "theory/bv/theory_bv.h"
#include "theory/output_channel.h"
#include "theory/term_registration_visitor.h"
#include "theory/uf/equality_engine.h"
#include "theory/uf/theory_uf.h"
#include "theory/valuation.h"
#include "util/hash.h"
#include "util/proof.h"


namespace CVC4 {

unsigned CVC4::LetCount::counter = 0;
static unsigned LET_COUNT = 1;

TheoryProofEngine::TheoryProofEngine()
  : d_registrationCache()
  , d_theoryProofTable()
{
  d_theoryProofTable[theory::THEORY_BOOL] = new LFSCBooleanProof(this);
}

TheoryProofEngine::~TheoryProofEngine() {
  TheoryProofTable::iterator it = d_theoryProofTable.begin();
  TheoryProofTable::iterator end = d_theoryProofTable.end();
  for (; it != end; ++it) {
    delete it->second;
  }
}

void TheoryProofEngine::registerTheory(theory::Theory* th) {
  if (th) {
    theory::TheoryId id = th->getId();
    if(d_theoryProofTable.find(id) == d_theoryProofTable.end()) {

      Trace("pf::tp") << "TheoryProofEngine::registerTheory: " << id << std::endl;

      if (id == theory::THEORY_UF) {
        d_theoryProofTable[id] = new LFSCUFProof((theory::uf::TheoryUF*)th, this);
        return;
      }

      if (id == theory::THEORY_BV) {
        BitVectorProof * bvp = new LFSCBitVectorProof((theory::bv::TheoryBV*)th, this);
        d_theoryProofTable[id] = bvp;
        ((theory::bv::TheoryBV*)th)->setProofLog( bvp );
        return;
      }

      if (id == theory::THEORY_ARRAY) {
        d_theoryProofTable[id] = new LFSCArrayProof((theory::arrays::TheoryArrays*)th, this);
        return;
      }

      if (id == theory::THEORY_ARITH) {
        d_theoryProofTable[id] = new LFSCArithProof((theory::arith::TheoryArith*)th, this);
        return;
      }

      // TODO other theories
    }
  }
}

TheoryProof* TheoryProofEngine::getTheoryProof(theory::TheoryId id) {
  // The UF theory handles queries for the Builtin theory.
  if (id == theory::THEORY_BUILTIN) {
    Debug("pf::tp") << "TheoryProofEngine::getTheoryProof: BUILTIN --> UF" << std::endl;
    id = theory::THEORY_UF;
  }

  Assert (d_theoryProofTable.find(id) != d_theoryProofTable.end());
  return d_theoryProofTable[id];
}

void TheoryProofEngine::markTermForFutureRegistration(Expr term, theory::TheoryId id) {
  d_exprToTheoryIds[term].insert(id);
}

void TheoryProofEngine::printConstantDisequalityProof(std::ostream& os, Expr c1, Expr c2) {
  LetMap emptyMap;

  os << "(trust_f (not (= _ ";
  printBoundTerm(c1, os, emptyMap);
  os << " ";
  printBoundTerm(c2, os, emptyMap);
  os << ")))";
}

void TheoryProofEngine::registerTerm(Expr term) {
  Debug("pf::tp") << "TheoryProofEngine::registerTerm: registering term: " << term << std::endl;

  if (d_registrationCache.count(term)) {
    return;
  }

  Debug("pf::tp") << "TheoryProofEngine::registerTerm: registering NEW term: " << term << std::endl;

  theory::TheoryId theory_id = theory::Theory::theoryOf(term);

  Debug("pf::tp") << "Term's theory( " << term << " ) = " << theory_id << std::endl;

  // don't need to register boolean terms
  if (theory_id == theory::THEORY_BUILTIN ||
      term.getKind() == kind::ITE) {
    for (unsigned i = 0; i < term.getNumChildren(); ++i) {
      registerTerm(term[i]);
    }
    d_registrationCache.insert(term);
    return;
  }

  if (!supportedTheory(theory_id)) return;

  // Register the term with its owner theory
  getTheoryProof(theory_id)->registerTerm(term);

  // A special case: the array theory needs to know of every skolem, even if
  // it belongs to another theory (e.g., a BV skolem)
  if (ProofManager::getSkolemizationManager()->isSkolem(term) && theory_id != theory::THEORY_ARRAY) {
    Debug("pf::tp") << "TheoryProofEngine::registerTerm: Special case: registering a non-array skolem: " << term << std::endl;
    getTheoryProof(theory::THEORY_ARRAY)->registerTerm(term);
  }

  d_registrationCache.insert(term);
}

theory::TheoryId TheoryProofEngine::getTheoryForLemma(const prop::SatClause* clause) {
  ProofManager* pm = ProofManager::currentPM();

  std::set<Node> nodes;
  for(unsigned i = 0; i < clause->size(); ++i) {
    prop::SatLiteral lit = (*clause)[i];
    Node node = pm->getCnfProof()->getAtom(lit.getSatVariable());
    Expr atom = node.toExpr();
    if (atom.isConst()) {
      Assert (atom == utils::mkTrue());
      continue;
    }

    nodes.insert(lit.isNegated() ? node.notNode() : node);
  }

  if (!pm->getCnfProof()->haveProofRecipe(nodes)) {
    // This lemma is not in the database. We only allow this in the special case of arithmetic with holes.
    if ((pm->getLogic() == "QF_UFLIA") || (pm->getLogic() == "QF_UFLRA")) {
      Debug("pf::tp") << "TheoryProofEngine::getTheoryForLemma: special hack for Arithmetic-with-holes support. "
                      << "Returning THEORY_ARITH" << std::endl;
      return theory::THEORY_ARITH;
    } else {
      Unreachable();
    }
  }

  theory::TheoryId owner = pm->getCnfProof()->getProofRecipe(nodes).getTheory();
  return owner;

  // if (pm->getLogic() == "QF_UF") return theory::THEORY_UF;
  // if (pm->getLogic() == "QF_BV") return theory::THEORY_BV;
  // if (pm->getLogic() == "QF_AX") return theory::THEORY_ARRAY;
  // if (pm->getLogic() == "ALL_SUPPORTED") return theory::THEORY_BV;

  // Debug("pf::tp") << "Unsupported logic (" << pm->getLogic() << ")" << std::endl;

  // Unreachable();
}

void LFSCTheoryProofEngine::bind(Expr term, LetMap& map, Bindings& let_order) {
  LetMap::iterator it = map.find(term);
  if (it != map.end()) {
    LetCount& count = it->second;
    count.increment();
    return;
  }
  for (unsigned i = 0; i < term.getNumChildren(); ++i) {
    bind(term[i], map, let_order);
  }
  unsigned new_id = LetCount::newId();
  map[term] = LetCount(new_id);
  let_order.push_back(LetOrderElement(term, new_id));
}

void LFSCTheoryProofEngine::printLetTerm(Expr term, std::ostream& os) {
  LetMap map;
  Bindings let_order;
  bind(term, map, let_order);
  std::ostringstream paren;
  for (unsigned i = 0; i < let_order.size(); ++i) {
    Expr current_expr = let_order[i].expr;
    unsigned let_id = let_order[i].id;
    LetMap::const_iterator it = map.find(current_expr);
    Assert (it != map.end());
    unsigned let_count = it->second.count;
    Assert(let_count);
    // skip terms that only appear once
    if (let_count <= LET_COUNT) {
      continue;
    }

    os << "(@ let"<<let_id << " ";
    printTheoryTerm(current_expr, os, map);
    paren <<")";
  }
  unsigned last_let_id = let_order.back().id;
  Expr last = let_order.back().expr;
  unsigned last_count = map.find(last)->second.count;
  if (last_count <= LET_COUNT) {
    printTheoryTerm(last, os, map);
  }
  else {
    os << " let"<< last_let_id;
  }
  os << paren.str();
}


void LFSCTheoryProofEngine::printTheoryTerm(Expr term, std::ostream& os, const LetMap& map) {
  theory::TheoryId theory_id = theory::Theory::theoryOf(term);
  // Debug("pf::tp") << std::endl << "LFSCTheoryProofEngine::printTheoryTerm: term = " << term
  //                    << ", theory_id = " << theory_id << std::endl;

  // boolean terms and ITEs are special because they
  // are common to all theories
  if (theory_id == theory::THEORY_BUILTIN ||
      term.getKind() == kind::ITE ||
      term.getKind() == kind::EQUAL) {
    printCoreTerm(term, os, map);
    return;
  }
  // dispatch to proper theory
  getTheoryProof(theory_id)->printOwnedTerm(term, os, map);
}

void LFSCTheoryProofEngine::printSort(Type type, std::ostream& os) {
  if (type.isSort()) {
    getTheoryProof(theory::THEORY_UF)->printOwnedSort(type, os);
    return;
  }
  if (type.isBitVector()) {
    getTheoryProof(theory::THEORY_BV)->printOwnedSort(type, os);
    return;
  }

  if (type.isArray()) {
    getTheoryProof(theory::THEORY_ARRAY)->printOwnedSort(type, os);
    return;
  }

  if (type.isInteger() || type.isReal()) {
    getTheoryProof(theory::THEORY_ARITH)->printOwnedSort(type, os);
    return;
  }

  if (type.isBoolean()) {
    getTheoryProof(theory::THEORY_BOOL)->printOwnedSort(type, os);
    return;
  }

  Unreachable();
}

void LFSCTheoryProofEngine::performExtraRegistrations() {
  ExprToTheoryIds::const_iterator it;
  for (it = d_exprToTheoryIds.begin(); it != d_exprToTheoryIds.end(); ++it) {
    if (d_registrationCache.count(it->first)) { // Only register if the term appeared
      TheoryIdSet::const_iterator theoryIt;
      for (theoryIt = it->second.begin(); theoryIt != it->second.end(); ++theoryIt) {
        Debug("pf::tp") << "\tExtra registration of term " << it->first << " with theory: " << *theoryIt << std::endl;
        Assert(supportedTheory(*theoryIt));
        getTheoryProof(*theoryIt)->registerTerm(it->first);
      }
    }
  }
}

void LFSCTheoryProofEngine::registerTermsFromAssertions() {
  ProofManager::assertions_iterator it = ProofManager::currentPM()->begin_assertions();
  ProofManager::assertions_iterator end = ProofManager::currentPM()->end_assertions();

  for(; it != end; ++it) {
    registerTerm(*it);
  }

  performExtraRegistrations();
}

void LFSCTheoryProofEngine::printAssertions(std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printAssertions called" << std::endl << std::endl;

  unsigned counter = 0;
  ProofManager::assertions_iterator it = ProofManager::currentPM()->begin_assertions();
  ProofManager::assertions_iterator end = ProofManager::currentPM()->end_assertions();

  for (; it != end; ++it) {
    Debug("pf::tp") << "printAssertions: assertion is: " << *it << std::endl;
    // FIXME: merge this with counter
    std::ostringstream name;
    name << "A" << counter++;

    ProofManager::currentPM()->registerUnrewrittenAssertion(*it, name.str());
    os << "(% " << name.str() << " (th_holds ";
    printLetTerm(*it,  os);
    os << ")\n";
    paren << ")";
  }
  //store map between assertion and counter
  // ProofManager::currentPM()->setAssertion( *it );
  Debug("pf::tp") << "LFSCTheoryProofEngine::printAssertions done" << std::endl << std::endl;
}

void LFSCTheoryProofEngine::printLemmaRewrites(NodePairSet& rewrites, std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printLemmaRewrites called" << std::endl << std::endl;

  NodePairSet::const_iterator it;

  for (it = rewrites.begin(); it != rewrites.end(); ++it) {
    Debug("pf::tp") << "printLemmaRewrites: " << it->first << " --> " << it->second << std::endl;

    std::ostringstream rewriteRule;
    rewriteRule << ".lrr" << d_assertionToRewrite.size();

    LetMap emptyMap;
    os << "(th_let_pf _ (trust_f (iff ";
    printBoundTerm(it->second.toExpr(), os, emptyMap);
    os << " ";
    printBoundTerm(it->first.toExpr(), os, emptyMap);
    os << ")) (\\ " << rewriteRule.str() << "\n";

    d_assertionToRewrite[it->first] = rewriteRule.str();
    Debug("pf::tp") << "d_assertionToRewrite[" << it->first << "] = " << rewriteRule.str() << std::endl;
    paren << "))";
  }

  Debug("pf::tp") << "LFSCTheoryProofEngine::printLemmaRewrites done" << std::endl << std::endl;
}

void LFSCTheoryProofEngine::printSortDeclarations(std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printSortDeclarations called" << std::endl << std::endl;

  TheoryProofTable::const_iterator it = d_theoryProofTable.begin();
  TheoryProofTable::const_iterator end = d_theoryProofTable.end();
  for (; it != end; ++it) {
    it->second->printSortDeclarations(os, paren);
  }

  Debug("pf::tp") << "LFSCTheoryProofEngine::printSortDeclarations done" << std::endl << std::endl;
}

void LFSCTheoryProofEngine::printTermDeclarations(std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printTermDeclarations called" << std::endl << std::endl;

  TheoryProofTable::const_iterator it = d_theoryProofTable.begin();
  TheoryProofTable::const_iterator end = d_theoryProofTable.end();
  for (; it != end; ++it) {
    it->second->printTermDeclarations(os, paren);
  }

  Debug("pf::tp") << "LFSCTheoryProofEngine::printTermDeclarations done" << std::endl << std::endl;
}

void LFSCTheoryProofEngine::printDeferredDeclarations(std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printDeferredDeclarations called" << std::endl;

  TheoryProofTable::const_iterator it = d_theoryProofTable.begin();
  TheoryProofTable::const_iterator end = d_theoryProofTable.end();
  for (; it != end; ++it) {
    it->second->printDeferredDeclarations(os, paren);
  }
}

void LFSCTheoryProofEngine::printAliasingDeclarations(std::ostream& os, std::ostream& paren) {
  Debug("pf::tp") << "LFSCTheoryProofEngine::printAliasingDeclarations called" << std::endl;

  TheoryProofTable::const_iterator it = d_theoryProofTable.begin();
  TheoryProofTable::const_iterator end = d_theoryProofTable.end();
  for (; it != end; ++it) {
    it->second->printAliasingDeclarations(os, paren);
  }
}

void LFSCTheoryProofEngine::dumpTheoryLemmas(const IdToSatClause& lemmas) {
  Debug("pf::dumpLemmas") << "(DEBUG) Dumping ALL theory lemmas" << std::endl << std::endl;

  ProofManager* pm = ProofManager::currentPM();
  for (IdToSatClause::const_iterator it = lemmas.begin(); it != lemmas.end(); ++it) {
    ClauseId id = it->first;
    Debug("pf::dumpLemmas") << "**** \tLemma ID = " << id << std::endl;
    const prop::SatClause* clause = it->second;
    std::set<Node> nodes;
    for(unsigned i = 0; i < clause->size(); ++i) {
      prop::SatLiteral lit = (*clause)[i];
      Node node = pm->getCnfProof()->getAtom(lit.getSatVariable());
      if (node.isConst()) {
        Assert (node.toExpr() == utils::mkTrue());
        continue;
      }
      nodes.insert(lit.isNegated() ? node.notNode() : node);
    }

    LemmaProofRecipe recipe = pm->getCnfProof()->getProofRecipe(nodes);
    recipe.dump("pf::dumpLemmas");
    // Debug("pf::dumpLemmas") << "\tOwner theory = " << recipe.getTheory() << std::endl;

    // std::set<Node>::iterator nodeIt;
    // Debug("pf::dumpLemmas") << "\tLiterals:" << std::endl;
    // for (nodeIt = nodes.begin(); nodeIt != nodes.end(); ++nodeIt) {
    //   Debug("pf::dumpLemmas") << "\t\t" << *nodeIt << std::endl;
    // }

    // if (recipe.simpleLemma()) {
    //   Debug("pf::dumpLemmas") << std::endl << "[Simple lemma]" << std::endl;
    // } else {
    //   recipe.dump("pf::dumpLemmas");
    // }
    // Debug("pf::dumpLemmas") << std::endl;
  }

  Debug("pf::dumpLemmas") << "(DEBUG) Theory lemma printing DONE" << std::endl << std::endl;
}

// TODO: this function should be moved into the BV prover.
void LFSCTheoryProofEngine::finalizeBvConflicts(const IdToSatClause& lemmas, std::ostream& os, std::ostream& paren) {
  // BitVector theory is special case: must know all
  // conflicts needed ahead of time for resolution
  // proof lemmas
  std::vector<Expr> bv_lemmas;
  // Warning() << "LFSCTheoryProofEngine::finalizeBvConflicts: total #conflicts = " << lemmas.size() << std::endl;
  // unsigned count = 1;

  for (IdToSatClause::const_iterator it = lemmas.begin(); it != lemmas.end(); ++it) {
    // if (count % 1000 == 0) {
    //   Warning() << "LFSCTheoryProofEngine::finalizeBvConflicts: working on lemma # " << count++ << std::endl;
    // }

    const prop::SatClause* clause = it->second;

    std::vector<Expr> conflict;
    std::set<Node> conflictNodes;
    for(unsigned i = 0; i < clause->size(); ++i) {
      prop::SatLiteral lit = (*clause)[i];
      Node node = ProofManager::currentPM()->getCnfProof()->getAtom(lit.getSatVariable());
      Expr atom = node.toExpr();
      if (atom.isConst()) {
        Assert (atom == utils::mkTrue() ||
                (atom == utils::mkFalse() && lit.isNegated()));
        continue;
      }
      Expr expr_lit = lit.isNegated() ? atom.notExpr() : atom;
      conflict.push_back(expr_lit);
      conflictNodes.insert(lit.isNegated() ? node.notNode() : node);
    }

    LemmaProofRecipe recipe = ProofManager::currentPM()->getCnfProof()->getProofRecipe(conflictNodes);

    // if (recipe.simpleLemma()) {
    //   if (recipe.getTheory() == theory::THEORY_BV) {
    //     bv_lemmas.push_back(utils::mkSortedExpr(kind::OR, conflict));
    //   }
    // } else {
    unsigned numberOfSteps = recipe.getNumSteps();

    prop::SatClause currentClause = *clause;
    std::vector<Expr> currentClauseExpr = conflict;

    for (unsigned i = 0; i < numberOfSteps; ++i) {
      const LemmaProofRecipe::ProofStep* currentStep = recipe.getStep(i);

      if (currentStep->getTheory() != theory::THEORY_BV) {
        continue;
      }

      currentClause = *clause;
      currentClauseExpr = conflict;

      for (unsigned j = 0; j < i; ++j) {
        // Literals already used in previous steps need to be negated
        Node previousLiteralNode = recipe.getStep(j)->getLiteral();
        Node previousLiteralNodeNegated = previousLiteralNode.negate();
        prop::SatLiteral previousLiteralNegated =
          ProofManager::currentPM()->getCnfProof()->getLiteral(previousLiteralNodeNegated);

        currentClause.push_back(previousLiteralNegated);
        currentClauseExpr.push_back(previousLiteralNodeNegated.toExpr());
      }

      // If we're in the final step, the last literal is Null and should not be added.
      // Otherwise, the current literal does NOT need to be negated
      Node currentLiteralNode = currentStep->getLiteral();

      if (currentLiteralNode != Node()) {
        prop::SatLiteral currentLiteral =
          ProofManager::currentPM()->getCnfProof()->getLiteral(currentLiteralNode);

        currentClause.push_back(currentLiteral);
        currentClauseExpr.push_back(currentLiteralNode.toExpr());
      }

      bv_lemmas.push_back(utils::mkSortedExpr(kind::OR, currentClauseExpr));
    }

      // if (recipe.getTheory() == theory::THEORY_BV) {
      //   // And now the final step
      //   currentClause = *clause;
      //   currentClauseExpr = conflict;

      //   for (unsigned i = 0; i < numberOfSteps; ++i) {
      //     // All literals already used in previous steps need to be negated
      //     Node previousLiteralNode = recipe.getStep(i)->getLiteral();
      //     Node previousLiteralNodeNegated = previousLiteralNode.negate();
      //     prop::SatLiteral previousLiteralNegated =
      //       ProofManager::currentPM()->getCnfProof()->getLiteral(previousLiteralNodeNegated);

      //     currentClause.push_back(previousLiteralNegated);
      //     currentClauseExpr.push_back(previousLiteralNodeNegated.toExpr());
      //   }

      //   bv_lemmas.push_back(utils::mkSortedExpr(kind::OR, currentClauseExpr));
      // }
  }
  //}

  BitVectorProof* bv = ProofManager::getBitVectorProof();
  bv->finalizeConflicts(bv_lemmas);

  bv->printResolutionProof(os, paren);
}

void LFSCTheoryProofEngine::printTheoryLemmas(const IdToSatClause& lemmas,
                                              std::ostream& os,
                                              std::ostream& paren) {
  os << " ;; Theory Lemmas \n";

  Debug("pf::tp") << "LFSCTheoryProofEngine::printTheoryLemmas: starting" << std::endl;

  if (Debug.isOn("pf::dumpLemmas")) {
    dumpTheoryLemmas(lemmas);
  }

  Debug("gk::temp") << "finalizeConflicts starting" << std::endl;
  // Warning() << "finalizeConflicts starting" << std::endl;

  finalizeBvConflicts(lemmas, os, paren);

  // Warning() << "finalizeConflicts done" << std::endl;

  if (options::bitblastMode() == theory::bv::BITBLAST_MODE_EAGER) {
    Assert (lemmas.size() == 1);
    // nothing more to do (no combination with eager so far)
    return;
  }

  ProofManager* pm = ProofManager::currentPM();

  Debug("pf::tp") << "LFSCTheoryProofEngine::printTheoryLemmas: printing lemmas..." << std::endl;

  for (IdToSatClause::const_iterator it = lemmas.begin(); it != lemmas.end(); ++it) {
    ClauseId id = it->first;
    const prop::SatClause* clause = it->second;

    Debug("pf::tp") << "LFSCTheoryProofEngine::printTheoryLemmas: printing lemma. ID = " << id << std::endl;

    std::vector<Expr> clause_expr;
    std::set<Node> clause_expr_nodes;
    for(unsigned i = 0; i < clause->size(); ++i) {
      prop::SatLiteral lit = (*clause)[i];
      Node node = pm->getCnfProof()->getAtom(lit.getSatVariable());
      Expr atom = node.toExpr();
      if (atom.isConst()) {
        Assert (atom == utils::mkTrue());
        continue;
      }
      Expr expr_lit = lit.isNegated() ? atom.notExpr(): atom;
      clause_expr.push_back(expr_lit);
      clause_expr_nodes.insert(lit.isNegated() ? node.notNode() : node);
    }

    LemmaProofRecipe recipe = pm->getCnfProof()->getProofRecipe(clause_expr_nodes);

    if (recipe.simpleLemma()) {
      // In a simple lemma, there will be no propositional resolution in the end

      Debug("pf::tp") << "Simple lemma" << std::endl;
      // printing clause as it appears in resolution proof
      os << "(satlem _ _ ";
      std::ostringstream clause_paren;

      pm->getCnfProof()->printClause(*clause, os, clause_paren);

      // query appropriate theory for proof of clause
      theory::TheoryId theory_id = getTheoryForLemma(clause);
      Debug("pf::tp") << "Get theory lemma from " << theory_id << "..." << std::endl;
      Debug("theory-proof-debug") << ";; Get theory lemma from " << theory_id << "..." << std::endl;

      //      getTheoryProof(theory_id)->printTheoryLemmaProof(clause_expr, os, paren);

      std::set<Node> missingAssertions = recipe.getMissingAssertionsForStep(0);
      if (!missingAssertions.empty()) {
        Debug("pf::tp") << "Have missing assertions for this simple lemma!" << std::endl;
      }

      std::set<Node>::const_iterator missingAssertion;
      for (missingAssertion = missingAssertions.begin();
           missingAssertion != missingAssertions.end();
           ++missingAssertion) {

        Debug("pf::tp") << "Working on missing assertion: " << *missingAssertion << std::endl;

        Assert(recipe.wasRewritten(missingAssertion->negate()));
        Node explanation = recipe.getExplanation(missingAssertion->negate()).negate();

        Debug("pf::tp") << "Found explanation: " << explanation << std::endl;

        // We have a missing assertion.
        //     rewriteIt->first is the assertion after the rewrite (the explanation),
        //     rewriteIt->second is the original assertion that needs to be fed into the theory.

        bool found = false;
        unsigned k;
        for (k = 0; k < clause_expr.size(); ++k) {
          if (clause_expr[k] == explanation.toExpr()) {
            found = true;
            break;
          }
        }

        Assert(found);

        Debug("pf::tp") << "Replacing theory assertion "
                        << clause_expr[k]
                        << " with "
                        << *missingAssertion
                        << std::endl;

        clause_expr[k] = missingAssertion->toExpr();

        std::ostringstream rewritten;
        Debug("gk::temp2") << ";; explanation = " << explanation << std::endl;
        Debug("gk::temp2") << ";; missingAssertion = " << *missingAssertion << std::endl;

        bool assertionPol = (missingAssertion->getKind() != kind::NOT);
        bool explanationPol = (explanation.getKind() != kind::NOT);

        Debug("gk::temp2") << ";; assertionPol = " << assertionPol << ", explanationPol == " << explanationPol << std::endl;

        // rewritten << "(or_elim_1 _ _ ";
        // rewritten << pm->getLitName(explanation);
        // rewritten << " (iff_elim_1 _ _ ";
        // rewritten << d_assertionToRewrite[*missingAssertion];
        // rewritten << "))";

        rewritten << "(or_elim_1 _ _ ";
        rewritten << "(not_not_intro _ ";
        rewritten << pm->getLitName(explanation);
        rewritten << ") (iff_elim_1 _ _ ";
        rewritten << d_assertionToRewrite[missingAssertion->negate()];
        rewritten << "))";


        pm->d_rewriteFilters[pm->getLitName(*missingAssertion)] = rewritten.str();

        Debug("pf::tp") << "Setting a rewrite filter for this proof: " << std::endl
                        << pm->getLitName(*missingAssertion) << " --> " << rewritten.str()
                        << std::endl << std::endl;
      }

      getTheoryProof(theory_id)->printTheoryLemmaProof(clause_expr, os, paren);

      // Turn rewrite filter OFF
      pm->d_rewriteFilters.clear();

      Debug("pf::tp") << "Get theory lemma from " << theory_id << "... DONE!" << std::endl;
      // os << " (clausify_false trust)";
      os << clause_paren.str();
      os << "( \\ " << pm->getLemmaClauseName(id) <<"\n";
      paren << "))";
    }
    else {
      // This is a composite lemma

      unsigned numberOfSteps = recipe.getNumSteps();

      prop::SatClause currentClause = *clause;
      std::vector<Expr> currentClauseExpr = clause_expr;

      for (unsigned i = 0; i < numberOfSteps; ++i) {
        const LemmaProofRecipe::ProofStep* currentStep = recipe.getStep(i);

        currentClause = *clause;
        currentClauseExpr = clause_expr;

        for (unsigned j = 0; j < i; ++j) {
          // Literals already used in previous steps need to be negated
          Node previousLiteralNode = recipe.getStep(j)->getLiteral();
          Node previousLiteralNodeNegated = previousLiteralNode.negate();
          prop::SatLiteral previousLiteralNegated =
            ProofManager::currentPM()->getCnfProof()->getLiteral(previousLiteralNodeNegated);
          currentClause.push_back(previousLiteralNegated);
          currentClauseExpr.push_back(previousLiteralNodeNegated.toExpr());
        }

        // If the current literal is NULL, can ignore (final step)
        // Otherwise, the current literal does NOT need to be negated
        Node currentLiteralNode = currentStep->getLiteral();
        if (currentLiteralNode != Node()) {
          prop::SatLiteral currentLiteral =
            ProofManager::currentPM()->getCnfProof()->getLiteral(currentLiteralNode);

          currentClause.push_back(currentLiteral);
          currentClauseExpr.push_back(currentLiteralNode.toExpr());
        }

        os << "(satlem _ _ ";
        std::ostringstream clause_paren;

        pm->getCnfProof()->printClause(currentClause, os, clause_paren);

        // query appropriate theory for proof of clause
        theory::TheoryId theory_id = currentStep->getTheory();
        Debug("pf::tp") << "Get theory lemma from " << theory_id << "..." << std::endl;

        std::set<Node> missingAssertions = recipe.getMissingAssertionsForStep(i);
        if (!missingAssertions.empty()) {
          Debug("pf::tp") << "Have missing assertions for this step!" << std::endl;
        }

        // Turn rewrite filter ON
        std::set<Node>::const_iterator missingAssertion;
        for (missingAssertion = missingAssertions.begin();
             missingAssertion != missingAssertions.end();
             ++missingAssertion) {

          Debug("pf::tp") << "Working on missing assertion: " << *missingAssertion << std::endl;

          Assert(recipe.wasRewritten(missingAssertion->negate()));
          Node explanation = recipe.getExplanation(missingAssertion->negate()).negate();
          // Assert(recipe.wasRewritten(missingAssertion->negate()));
          // Node explanation = recipe.getExplanation(missingAssertion->negate()).negate();

          Debug("pf::tp") << "Found explanation: " << explanation << std::endl;

          // We have a missing assertion.
          //     rewriteIt->first is the assertion after the rewrite (the explanation),
          //     rewriteIt->second is the original assertion that needs to be fed into the theory.

          bool found = false;
          unsigned k;
          for (k = 0; k < currentClauseExpr.size(); ++k) {
            if (currentClauseExpr[k] == explanation.toExpr()) {
              found = true;
              break;
            }
          }

          Assert(found);

          Debug("pf::tp") << "Replacing theory assertion "
                          << currentClauseExpr[k]
                          << " with "
                          << *missingAssertion
                          << std::endl;

          currentClauseExpr[k] = missingAssertion->toExpr();

          std::ostringstream rewritten;
          rewritten << "(or_elim_1 _ _ ";
          rewritten << "(not_not_intro _ ";
          rewritten << pm->getLitName(explanation);
          rewritten << ") (iff_elim_1 _ _ ";
          rewritten << d_assertionToRewrite[missingAssertion->negate()];
          rewritten << "))";

          pm->d_rewriteFilters[pm->getLitName(*missingAssertion)] = rewritten.str();

          Debug("pf::tp") << "Setting a rewrite filter for this proof: " << std::endl
                          << pm->getLitName(*missingAssertion) << " --> " << rewritten.str()
                          << std::endl << std::endl;
        }

        getTheoryProof(theory_id)->printTheoryLemmaProof(currentClauseExpr, os, paren);

        // Turn rewrite filter OFF
        pm->d_rewriteFilters.clear();

        Debug("pf::tp") << "Get theory lemma from " << theory_id << "... DONE!" << std::endl;
        os << clause_paren.str();
        os << "( \\ " << pm->getLemmaClauseName(id) << "s" << i <<"\n";
        paren << "))";
      }

      // // And now the final step
      // currentClause = *clause;
      // currentClauseExpr = clause_expr;

      // for (unsigned i = 0; i < numberOfSteps; ++i) {
      //   // All literals already used in previous steps need to be negated
      //   Node previousLiteralNode = recipe.getStep(i)->getLiteral();

      //   Node previousLiteralNodeNegated = previousLiteralNode.negate();
      //   prop::SatLiteral previousLiteralNegated =
      //     ProofManager::currentPM()->getCnfProof()->getLiteral(previousLiteralNodeNegated);
      //   currentClause.push_back(previousLiteralNegated);
      //   currentClauseExpr.push_back(previousLiteralNodeNegated.toExpr());
      // }

      // os << "(satlem _ _ ";
      // std::ostringstream clause_paren;

      // pm->getCnfProof()->printClause(currentClause, os, clause_paren);

      // // query appropriate theory for proof of clause
      // theory::TheoryId theory_id = recipe.getTheory();
      // Debug("pf::tp") << "Get theory lemma from " << theory_id << "..." << std::endl;

      // // std::set<Node> missingAssertions = recipe.getMissingAssertionsForStep(numberOfSteps - 1);
      // // if (!missingAssertions.empty()) {
      // //   Debug("pf::tp") << "Have missing assertions for this step!" << std::endl;
      // // }

      // // // Turn rewrite filter ON
      // // std::set<Node>::const_iterator missingAssertion;
      // // for (missingAssertion = missingAssertions.begin();
      // //      missingAssertion != missingAssertions.end();
      // //      ++missingAssertion) {

      // //   Debug("pf::tp") << "Working on missing assertion: " << *missingAssertion << std::endl;

      // //   Assert(recipe.wasRewritten(*missingAssertion));
      // //   Node explanation = recipe.getExplanation(*missingAssertion);

      // //   Debug("pf::tp") << "Found explanation: " << explanation << std::endl;

      // //   // We have a missing assertion.
      // //   //     rewriteIt->first is the assertion after the rewrite (the explanation),
      // //   //     rewriteIt->second is the original assertion that needs to be fed into the theory.

      // //   bool found = false;
      // //   unsigned k;
      // //   for (k = 0; k < currentClauseExpr.size(); ++k) {
      // //     if (currentClauseExpr[k] == explanation.negate().toExpr()) {
      // //       found = true;
      // //       break;
      // //     }
      // //   }

      // //   Assert(found);

      // //   Debug("pf::tp") << "Replacing theory assertion "
      // //                   << currentClauseExpr[k]
      // //                   << " with "
      // //                   << *missingAssertion
      // //                   << std::endl;

      // //   currentClauseExpr[k] = missingAssertion->toExpr();

      // //   std::ostringstream rewritten;
      // //   rewritten << "(or_elim_1 _ _ ";
      // //   rewritten << "(not_not_intro _ ";
      // //   rewritten << pm->getLitName(explanation);
      // //   rewritten << "(iff_elim1 _ _ ";
      // //   rewritten << missingAssertion->negate();
      // //   rewritten << ")))";

      // //   pm->d_rewriteFilters[pm->getLitName(explanation)] = rewritten.str();

      // //   Debug("pf::tp") << "Setting a rewrite filter for this proof: " << std::endl
      // //                   << pm->getLitName(explanation) << " --> " << rewritten.str()
      // //                   << std::endl << std::endl;
      // // }

      // getTheoryProof(theory_id)->printTheoryLemmaProof(currentClauseExpr, os, paren);
      // Debug("pf::tp") << "Get theory lemma from " << theory_id << "... DONE!" << std::endl;
      // // os << " (clausify_false trust)";
      // os << clause_paren.str();
      // os << "( \\ " << pm->getLemmaClauseName(id) << "s" << numberOfSteps << "\n";
      // paren << "))";

      // Now we need to do propositional resolution on the steps to get the "real" lemma

      Assert(numberOfSteps >= 2);

      os << "(satlem_simplify _ _ _ ";
      for (unsigned i = 0; i < numberOfSteps - 1; ++i) {
        // Resolve step i with step i + 1
        if (recipe.getStep(i)->getLiteral().getKind() == kind::NOT) {
          os << "(Q _ _ ";
        } else {
          os << "(R _ _ ";
        }

        os << pm->getLemmaClauseName(id) << "s" << i;
        os << " ";
      }

      os << pm->getLemmaClauseName(id) << "s" << numberOfSteps - 1 << " ";

      prop::SatLiteral v;
      for (int i = numberOfSteps - 2; i >= 0; --i) {
        v = ProofManager::currentPM()->getCnfProof()->getLiteral(recipe.getStep(i)->getLiteral());
        os << ProofManager::getVarName(v.getSatVariable(), "") << ") ";
      }

      os << "( \\ " << pm->getLemmaClauseName(id) << "\n";
      paren << "))";
    }
  }
}

void LFSCTheoryProofEngine::printBoundTerm(Expr term, std::ostream& os, const LetMap& map) {
  // Debug("pf::tp") << "LFSCTheoryProofEngine::printBoundTerm( " << term << " ) " << std::endl;

  LetMap::const_iterator it = map.find(term);
  if (it != map.end()) {
    unsigned id = it->second.id;
    unsigned count = it->second.count;
    if (count > LET_COUNT) {
      os <<"let"<<id;
      return;
    }
  }

  printTheoryTerm(term, os, map);
}

void LFSCTheoryProofEngine::printCoreTerm(Expr term, std::ostream& os, const LetMap& map) {
  if (term.isVariable()) {
    os << ProofManager::sanitize(term);
    return;
  }

  Kind k = term.getKind();

  switch(k) {
  case kind::ITE:
    os << (term.getType().isBoolean() ? "(ifte ": "(ite _ ");

    printBoundTerm(term[0], os, map);
    os << " ";
    printBoundTerm(term[1], os, map);
    os << " ";
    printBoundTerm(term[2], os, map);
    os << ")";
    return;

  case kind::EQUAL:
    os << "(";
    os << "= ";
    printSort(term[0].getType(), os);
    printBoundTerm(term[0], os, map);
    os << " ";
    printBoundTerm(term[1], os, map);
    os << ")";
    return;

  case kind::DISTINCT:
    // Distinct nodes can have any number of chidlren.
    Assert (term.getNumChildren() >= 2);

    if (term.getNumChildren() == 2) {
      os << "(not (= ";
      printSort(term[0].getType(), os);
      printBoundTerm(term[0], os, map);
      os << " ";
      printBoundTerm(term[1], os, map);
      os << "))";
    } else {
      unsigned numOfPairs = term.getNumChildren() * (term.getNumChildren() - 1) / 2;
      for (unsigned i = 1; i < numOfPairs; ++i) {
        os << "(and ";
      }

      for (unsigned i = 0; i < term.getNumChildren(); ++i) {
        for (unsigned j = i + 1; j < term.getNumChildren(); ++j) {
          if ((i != 0) || (j != 1)) {
            os << "(not (= ";
            printSort(term[0].getType(), os);
            printBoundTerm(term[i], os, map);
            os << " ";
            printBoundTerm(term[j], os, map);
            os << ")))";
          } else {
            os << "(not (= ";
            printSort(term[0].getType(), os);
            printBoundTerm(term[0], os, map);
            os << " ";
            printBoundTerm(term[1], os, map);
            os << "))";
          }
        }
      }
    }

    return;

  case kind::CHAIN: {
    // LFSC doesn't allow declarations with variable numbers of
    // arguments, so we have to flatten chained operators, like =.
    Kind op = term.getOperator().getConst<Chain>().getOperator();
    size_t n = term.getNumChildren();
    std::ostringstream paren;
    for(size_t i = 1; i < n; ++i) {
      if(i + 1 < n) {
        os << "(" << utils::toLFSCKind(kind::AND) << " ";
        paren << ")";
      }
      os << "(" << utils::toLFSCKind(op) << " ";
      printBoundTerm(term[i - 1], os, map);
      os << " ";
      printBoundTerm(term[i], os, map);
      os << ")";
      if(i + 1 < n) {
        os << " ";
      }
    }
    os << paren.str();
    return;
  }

  default:
    Unhandled(k);
  }

}

void TheoryProof::printTheoryLemmaProof(std::vector<Expr>& lemma, std::ostream& os, std::ostream& paren) {
    if (options::eagerUfProofs() || options::eagerArrayProofs()) {
    // Eager proof mode.
    Debug("pf::eager") << std::endl << "TheoryProof::printTheoryLemmaProof called in eager proof mode" << std::endl;

    std::set<Node> conflict;
    for (unsigned i = 0; i < lemma.size(); ++i) {
      conflict.insert(Node(lemma[i]).negate());
    }

    Debug("pf::eager") << "conflict = " << std::endl << "\t";
    std::set<Node>::const_iterator it;
    for (it = conflict.begin(); it != conflict.end(); ++it) {
      Debug("pf::eager") << *it << " ";
    }
    Debug("pf::eager") << std::endl;

    if (ProofManager::currentPM()->d_eagerConflictToProof.find(conflict) != ProofManager::currentPM()->d_eagerConflictToProof.end()) {
      Debug("pf::eager") << "Conflict node exists in the database. Printing proof" << std::endl;
      ProofManager::currentPM()->d_eagerConflictToProof[conflict]->toStream(os);
      Debug("pf::eager") << "Done printing proof" << std::endl;
      return;
    } else {
      Debug("pf::eager") << "Conflict DID NOT exist in the database (theory lemma?). Doing a lazy proof." << std::endl;
    }
  }

  //default method for replaying proofs: assert (negated) literals back to a fresh copy of the theory
  Assert( d_theory!=NULL );
  context::UserContext fakeContext;
  ProofOutputChannel oc;
  theory::Valuation v(NULL);
  //make new copy of theory
  theory::Theory* th;
  Trace("theory-proof-debug") << ";; Print theory lemma proof, theory id = " << d_theory->getId() << std::endl;
  if(d_theory->getId()==theory::THEORY_UF) {
    th = new theory::uf::TheoryUF(&fakeContext, &fakeContext, oc, v,
                                  ProofManager::currentPM()->getLogicInfo(),
                                  "replay::");
  } else if(d_theory->getId()==theory::THEORY_BV) {
    Assert(!options::eagerBvProofs()); // Eager BV proofs are handled in bv_proof.
    th = new theory::bv::TheoryBV(&fakeContext, &fakeContext, oc, v,
                                  ProofManager::currentPM()->getLogicInfo(),
                                  "replay::");
  } else if(d_theory->getId()==theory::THEORY_ARRAY) {
    th = new theory::arrays::TheoryArrays(&fakeContext, &fakeContext, oc, v,
                                          ProofManager::currentPM()->getLogicInfo(),
                                          "replay::");
  } else if (d_theory->getId() == theory::THEORY_ARITH) {
    Trace("theory-proof-debug") << "Arith proofs currently not supported. Use 'trust'" << std::endl;
    os << " (clausify_false trust)";
    return;
  } else {
    InternalError(std::string("can't generate theory-proof for ") + ProofManager::currentPM()->getLogic());
  }

  Debug("pf::tp") << "TheoryProof::printTheoryLemmaProof - calling th->ProduceProofs()" << std::endl;
  th->produceProofs();
  Debug("pf::tp") << "TheoryProof::printTheoryLemmaProof - th->ProduceProofs() DONE" << std::endl;

  MyPreRegisterVisitor preRegVisitor(th);
  for( unsigned i=0; i<lemma.size(); i++ ){
    Node lit = Node::fromExpr( lemma[i] ).negate();
    Trace("pf::tp") << "; preregistering and asserting " << lit << std::endl;
    NodeVisitor<MyPreRegisterVisitor>::run(preRegVisitor, lit);
    th->assertFact(lit, false);
  }

  Debug("pf::tp") << "TheoryProof::printTheoryLemmaProof - calling th->check()" << std::endl;
  th->check(theory::Theory::EFFORT_FULL);
  Debug("pf::tp") << "TheoryProof::printTheoryLemmaProof - th->check() DONE" << std::endl;

  if(oc.d_conflict.isNull()) {
    Trace("pf::tp") << "; conflict is null" << std::endl;
    Assert(!oc.d_lemma.isNull());
    Trace("pf::tp") << "; ++ but got lemma: " << oc.d_lemma << std::endl;

    // Original, as in Liana's branch
    // Trace("pf::tp") << "; asserting " << oc.d_lemma[1].negate() << std::endl;
    // th->assertFact(oc.d_lemma[1].negate(), false);
    // th->check(theory::Theory::EFFORT_FULL);

    // Altered version, to handle OR lemmas

    if (oc.d_lemma.getKind() == kind::OR) {
      Debug("pf::tp") << "OR lemma. Negating each child separately" << std::endl;
      for (unsigned i = 0; i < oc.d_lemma.getNumChildren(); ++i) {
        if (oc.d_lemma[i].getKind() == kind::NOT) {
          Trace("pf::tp") << ";     asserting fact: " << oc.d_lemma[i][0] << std::endl;
          th->assertFact(oc.d_lemma[i][0], false);
        }
        else {
          Trace("pf::tp") << ";     asserting fact: " << oc.d_lemma[i].notNode() << std::endl;
          th->assertFact(oc.d_lemma[i].notNode(), false);
        }
      }
    }
    else {
      Unreachable();

      Assert(oc.d_lemma.getKind() == kind::NOT);
      Debug("pf::tp") << "NOT lemma" << std::endl;
      Trace("pf::tp") << ";     asserting fact: " << oc.d_lemma[0] << std::endl;
      th->assertFact(oc.d_lemma[0], false);
    }

    // Trace("pf::tp") << "; ++ but got lemma: " << oc.d_lemma << std::endl;
    // Trace("pf::tp") << "; asserting " << oc.d_lemma[1].negate() << std::endl;
    // th->assertFact(oc.d_lemma[1].negate(), false);

    //
    th->check(theory::Theory::EFFORT_FULL);
  }
  Debug("pf::tp") << "Calling   oc.d_proof->toStream(os)" << std::endl;
  oc.d_proof->toStream(os);
  Debug("pf::tp") << "Calling   oc.d_proof->toStream(os) -- DONE!" << std::endl;

  Debug("pf::tp") << "About to delete the theory solver used for proving the lemma... " << std::endl;
  delete th;
  Debug("pf::tp") << "About to delete the theory solver used for proving the lemma: DONE! " << std::endl;
}

bool TheoryProofEngine::supportedTheory(theory::TheoryId id) {
  return (id == theory::THEORY_ARRAY ||
          id == theory::THEORY_ARITH ||
          id == theory::THEORY_BV ||
          id == theory::THEORY_UF ||
          id == theory::THEORY_BOOL);
}

BooleanProof::BooleanProof(TheoryProofEngine* proofEngine)
  : TheoryProof(NULL, proofEngine)
{}

void BooleanProof::registerTerm(Expr term) {
  Assert (term.getType().isBoolean());

  if (term.isVariable() && d_declarations.find(term) == d_declarations.end()) {
    d_declarations.insert(term);
    return;
  }
  for (unsigned i = 0; i < term.getNumChildren(); ++i) {
    d_proofEngine->registerTerm(term[i]);
  }
}

void LFSCBooleanProof::printOwnedTerm(Expr term, std::ostream& os, const LetMap& map) {
  Assert (term.getType().isBoolean());
  if (term.isVariable()) {
    os << "(p_app " << ProofManager::sanitize(term) <<")";
    return;
  }

  Kind k = term.getKind();
  switch(k) {
  case kind::OR:
  case kind::AND:
  case kind::XOR:
  case kind::IFF:
  case kind::IMPLIES:
  case kind::NOT:
    // print the Boolean operators
    os << "(" << utils::toLFSCKind(k);
    if(term.getNumChildren() > 2) {
      // LFSC doesn't allow declarations with variable numbers of
      // arguments, so we have to flatten these N-ary versions.
      std::ostringstream paren;
      os << " ";
      for (unsigned i = 0; i < term.getNumChildren(); ++i) {
        d_proofEngine->printBoundTerm(term[i], os, map);
        os << " ";
        if(i < term.getNumChildren() - 2) {
          os << "(" << utils::toLFSCKind(k) << " ";
          paren << ")";
        }
      }
      os << paren.str() << ")";
    } else {
      // this is for binary and unary operators
      for (unsigned i = 0; i < term.getNumChildren(); ++i) {
        os << " ";
        d_proofEngine->printBoundTerm(term[i], os, map);
      }
      os << ")";
    }
    return;

  case kind::CONST_BOOLEAN:
    os << (term.getConst<bool>() ? "true" : "false");
    return;

  default:
    Unhandled(k);
  }

}

void LFSCBooleanProof::printOwnedSort(Type type, std::ostream& os) {
  Assert (type.isBoolean());
  os << "Bool";
}

void LFSCBooleanProof::printSortDeclarations(std::ostream& os, std::ostream& paren) {
  // Nothing to do here at this point.
}

void LFSCBooleanProof::printTermDeclarations(std::ostream& os, std::ostream& paren) {
  for (ExprSet::const_iterator it = d_declarations.begin(); it != d_declarations.end(); ++it) {
    Expr term = *it;

    os << "(% " << ProofManager::sanitize(term) << " (term ";
    printSort(term.getType(), os);
    os <<")\n";
    paren <<")";
  }
}

void LFSCBooleanProof::printDeferredDeclarations(std::ostream& os, std::ostream& paren) {
  // Nothing to do here at this point.
}

void LFSCBooleanProof::printAliasingDeclarations(std::ostream& os, std::ostream& paren) {
  // Nothing to do here at this point.
}

void LFSCBooleanProof::printTheoryLemmaProof(std::vector<Expr>& lemma,
                                             std::ostream& os,
                                             std::ostream& paren) {
  Unreachable("No boolean lemmas yet!");
}

} /* namespace CVC4 */
