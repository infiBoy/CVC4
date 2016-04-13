/*********************                                                        */
/*! \file lemma_proof.h
** \verbatim
**
** \brief A class for recoding the steps required in order to prove a theory lemma.
**
** A class for recoding the steps required in order to prove a theory lemma.
**
**/

#include "cvc4_private.h"

#ifndef __CVC4__LEMMA_PROOF_H
#define __CVC4__LEMMA_PROOF_H

#include "expr/expr.h"
#include "proof/clause_id.h"
#include "prop/sat_solver_types.h"
#include "util/proof.h"
#include "expr/node.h"

namespace CVC4 {

class LemmaProofRecipe {
public:
  class ProofStep {
  public:
    ProofStep(theory::TheoryId theory, Node literalToProve);
    theory::TheoryId getTheory() const;
    Node getLiteral() const;
    void addAssumption(const Node& assumption);
    std::set<Node> getAssumptions() const;

  private:
    theory::TheoryId d_theory;
    Node d_literalToProve;
    std::set<Node> d_assumptions;
  };

  //* The lemma assertions and owner */
  void addAssertion(Node assertion);
  std::set<Node> getAssertions() const;
  void setTheory(theory::TheoryId theory);
  theory::TheoryId getTheory() const;

  //* Rewrite rules */
  typedef std::map<Node, Node>::const_iterator RewriteIterator;
  RewriteIterator rewriteBegin() const;
  RewriteIterator rewriteEnd() const;

  void addRewriteRule(Node assertion, Node explanation);
  bool wasRewritten(Node assertion) const;
  Node getExplanation(Node assertion) const;

  //* Proof Steps */
  void addStep(ProofStep& proofStep);
  ProofStep getStep(unsigned index) const;
  unsigned getNumSteps() const;
  std::set<Node> getMissingAssertionsForStep(unsigned index) const;
  bool simpleLemma() const;
  bool compositeLemma() const;

  void dump(const char *tag) const;
  bool operator<(const LemmaProofRecipe& other) const;

private:
  //* The list of assertions for this lemma */
  std::set<Node> d_assertions;

  //* The various steps needed to derive the empty clause */
  std::list<ProofStep> d_proofSteps;

  //* The owner theory. If the proof has steps, this theory makes the final step */
  theory::TheoryId d_theory;

  //* A map from assertions to their rewritten explanations (toAssert --> toExplain) */
  std::map<Node, Node> d_assertionToExplanation;
};

} /* CVC4 namespace */

#endif /* __CVC4__LEMMA_PROOF_H */