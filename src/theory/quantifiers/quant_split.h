/*********************                                                        */
/*! \file quant_split.h
 ** \verbatim
 ** Original author: Andrew Reynolds
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief dynamic quantifiers splitting
 **/

#include "cvc4_private.h"

#ifndef __CVC4__THEORY__QUANT_SPLIT_H
#define __CVC4__THEORY__QUANT_SPLIT_H

#include "theory/quantifiers_engine.h"
#include "context/cdo.h"

namespace CVC4 {
namespace theory {
namespace quantifiers {

class QuantDSplit : public QuantifiersModule {
  typedef context::CDHashSet<Node, NodeHashFunction> NodeSet;
private:
  /** list of relevant quantifiers asserted in the current context */
  std::map< Node, int > d_quant_to_reduce;
  /** whether we have instantiated quantified formulas */
  NodeSet d_added_split;
public:
  QuantDSplit( QuantifiersEngine * qe, context::Context* c );
  /** determine whether this quantified formula will be reduced */
  void preRegisterQuantifier( Node q );
  
  /* whether this module needs to check this round */
  bool needsCheck( Theory::Effort e );
  /* Call during quantifier engine's check */
  void check( Theory::Effort e, unsigned quant_e );
  /* Called for new quantifiers */
  void registerQuantifier( Node q ) {}
  void assertNode( Node n ) {}
  /** Identify this module (for debugging, dynamic configuration, etc..) */
  std::string identify() const { return "QuantDSplit"; }
};

}
}
}

#endif
