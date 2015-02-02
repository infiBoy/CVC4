/*********************                                                        */
/*! \file theory_datatypes.cpp
 ** \verbatim
 ** Original author: Andrew Reynolds
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief Implementation of sygus utilities for theory of datatypes
 **
 ** Implementation of sygus utilities for theory of datatypes
 **/


#include "theory/datatypes/datatypes_sygus.h"
#include "theory/datatypes/datatypes_rewriter.h"
#include "expr/node_manager.h"
#include "theory/bv/theory_bv_utils.h"
#include "util/bitvector.h"
#include "smt/smt_engine_scope.h"

#include "theory/quantifiers/options.h"

using namespace CVC4;
using namespace CVC4::kind;
using namespace CVC4::context;
using namespace CVC4::theory;
using namespace CVC4::theory::datatypes;

void SygusSplit::getSygusSplits( Node n, const Datatype& dt, std::vector< Node >& splits, std::vector< Node >& lemmas ) {
  Assert( dt.isSygus() );
  if( d_splits.find( n )==d_splits.end() ){
    Trace("sygus-split") << "Get sygus splits " << n << std::endl;
    //get the kinds for child datatype
    TypeNode tnn = n.getType();
    registerSygusType( tnn );

    //get parent information, if possible
    int csIndex = -1;
    int sIndex = -1;
    Node arg1;
    TypeNode tn1;
    TypeNode tnnp;
    Node ptest;
    if( n.getKind()==APPLY_SELECTOR_TOTAL ){
      Node op = n.getOperator();
      Expr selectorExpr = op.toExpr();
      const Datatype& pdt = Datatype::datatypeOf(selectorExpr);
      Assert( pdt.isSygus() );
      csIndex = Datatype::cindexOf(selectorExpr);
      sIndex = Datatype::indexOf(selectorExpr);
      tnnp = n[0].getType();
      //register the constructors that are redundant children of argument sIndex of constructor index csIndex of dt
      registerSygusTypeConstructorArg( tnn, dt, tnnp, pdt, csIndex, sIndex );

      if( options::sygusNormalFormArg() ){
        if( sIndex==1 && pdt[csIndex].getNumArgs()==2 ){
          arg1 = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( pdt[csIndex][0].getSelector() ), n[0] );
          tn1 = arg1.getType();
          if( !DatatypesRewriter::isTypeDatatype( tn1 ) ){
            arg1 = Node::null();
          }
        }
      }
      // we are splitting on a term that may later have no semantics : guard this case
      ptest = DatatypesRewriter::mkTester( n[0], csIndex, pdt );
      Trace("sygus-split-debug") << "Parent guard : " << ptest << std::endl;
    }

    std::vector< Node > ptest_c;
    bool narrow = false;
    for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
      Trace("sygus-split-debug2") << "Add split " << n << " : constructor " << dt[i].getName() << " : ";
      Assert( d_sygus_nred.find( tnn )!=d_sygus_nred.end() );
      bool addSplit = d_sygus_nred[tnn][i];
      if( addSplit ){
        if( csIndex!=-1 ){
          Assert( d_sygus_pc_nred[tnn][csIndex].find( sIndex )!=d_sygus_pc_nred[tnn][csIndex].end() );
          addSplit = d_sygus_pc_nred[tnn][csIndex][sIndex][i];
        }
        if( addSplit ){
          std::vector< Node > test_c;
          Node test = DatatypesRewriter::mkTester( n, i, dt );
          test_c.push_back( test );
          //check if we can strengthen the first argument
          if( !arg1.isNull() ){
            const Datatype& dt1 = ((DatatypeType)(tn1).toType()).getDatatype();
            Kind k = d_util->getArgKind( tnnp, csIndex );
            //size comparison for arguments (if necessary)
            Node sz_leq;
            if( tn1==tnn && d_util->isComm( k ) ){
              sz_leq = NodeManager::currentNM()->mkNode( LEQ, NodeManager::currentNM()->mkNode( DT_SIZE, n ), NodeManager::currentNM()->mkNode( DT_SIZE, arg1 ) );
            }
            std::map< int, std::vector< int > >::iterator it = d_sygus_pc_arg_pos[tnn][csIndex].find( i );
            if( it!=d_sygus_pc_arg_pos[tnn][csIndex].end() ){
              Assert( !it->second.empty() );
              std::vector< Node > lem_c;
              for( unsigned j=0; j<it->second.size(); j++ ){
                Node tt = DatatypesRewriter::mkTester( arg1, it->second[j], dt1 );
                //if commutative operator, and children have same constructor type, then first arg is larger than second arg
                if( it->second[j]==(int)i && !sz_leq.isNull() ){
                  tt = NodeManager::currentNM()->mkNode( AND, tt, sz_leq );
                }
                lem_c.push_back( tt );
              }
              Node argStr = lem_c.size()==1 ? lem_c[0] : NodeManager::currentNM()->mkNode( OR, lem_c );
              Trace("sygus-str") << "...strengthen corresponding first argument of " << test << " : " << argStr << std::endl;
              test_c.push_back( argStr );
              narrow = true;
            }else{
              if( !sz_leq.isNull() ){
                test_c.push_back( NodeManager::currentNM()->mkNode( OR, DatatypesRewriter::mkTester( arg1, i, dt1 ).negate(), sz_leq ) );
                narrow = true;
              }
            }
          }
          //other constraints on arguments
          Kind curr = d_util->getArgKind( tnn, i );
          if( curr!=UNDEFINED_KIND ){
            //ITE children must be distinct when properly typed
            if( curr==ITE ){
              if( getArgType( dt[i], 1 )==tnn && getArgType( dt[i], 2 )==tnn ){
                Node arg_ite1 = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[i][1].getSelector() ), n );
                Node arg_ite2 = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[i][2].getSelector() ), n );
                Node deq = arg_ite1.eqNode( arg_ite2 ).negate();
                Trace("sygus-str") << "...ite strengthen arguments " << deq << std::endl;
                test_c.push_back( deq );
                narrow = true;
              }
              //condition must be distinct from all parent ITE's
              Node curr = n;
              Node arg_itec = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[i][0].getSelector() ), n );
              while( curr.getKind()==APPLY_SELECTOR_TOTAL ){
                if( curr[0].getType()==tnn ){
                  int sIndexCurr = Datatype::indexOf( curr.getOperator().toExpr() );
                  int csIndexCurr = Datatype::cindexOf( curr.getOperator().toExpr() );
                  if( sIndexCurr!=0 && csIndexCurr==(int)i ){
                    Trace("sygus-ite") << "Parent ITE " << curr << " of " << n << std::endl;
                    Node arg_itecp = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[i][0].getSelector() ), curr[0] );
                    Node deq = arg_itec.eqNode( arg_itecp ).negate();
                    Trace("sygus-str") << "...ite strengthen condition " << deq << std::endl;
                    test_c.push_back( deq );
                    narrow = true;
                  }
                }
                curr = curr[0];
              }
            }
          }
          //add fairness constraint
          if( options::ceGuidedInstFair()==quantifiers::CEGQI_FAIR_DT_SIZE ){
            Node szl = NodeManager::currentNM()->mkNode( DT_SIZE, n );
            Node szr = NodeManager::currentNM()->mkNode( DT_SIZE, DatatypesRewriter::getInstCons( n, dt, i ) );
            szr = Rewriter::rewrite( szr );
            test_c.push_back( szl.eqNode( szr ) );
          }
          test = test_c.size()==1 ? test_c[0] : NodeManager::currentNM()->mkNode( AND, test_c );
          d_splits[n].push_back( test );
          Trace("sygus-split-debug2") << "SUCCESS" << std::endl;
          Trace("sygus-split-debug") << "Disjunct #" << d_splits[n].size() << " : " << test << std::endl;
        }else{
          Trace("sygus-split-debug2") << "redundant argument" << std::endl;
          narrow = true;
        }
      }else{
        Trace("sygus-split-debug2") << "redundant operator" << std::endl;
        narrow = true;
      }
      if( !ptest.isNull() ){
        ptest_c.push_back( DatatypesRewriter::mkTester( n, i, dt ) );
      }
    }
    if( narrow && !ptest.isNull() ){
      Node split = d_splits[n].empty() ? NodeManager::currentNM()->mkConst( false ) :
                                        ( d_splits[n].size()==1 ? d_splits[n][0] : NodeManager::currentNM()->mkNode( OR, d_splits[n] ) );
      if( !d_splits[n].empty() ){
        d_splits[n].clear();
        split = NodeManager::currentNM()->mkNode( AND, ptest, split );
      }
      d_splits[n].push_back( split );
      if( !ptest_c.empty() ){
        ptest = NodeManager::currentNM()->mkNode( AND, ptest.negate(), NodeManager::currentNM()->mkNode( OR, ptest_c ) );
      }
      d_splits[n].push_back( ptest );
    }else{
      Assert( !d_splits[n].empty() );
    }

  }
  //copy to splits
  splits.insert( splits.end(), d_splits[n].begin(), d_splits[n].end() );
}

void SygusSplit::registerSygusType( TypeNode tn ) {
  if( d_register.find( tn )==d_register.end() ){
    if( !DatatypesRewriter::isTypeDatatype( tn ) ){
      d_register[tn] = TypeNode::null();
    }else{
      const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
      Trace("sygus-split") << "Register type " << dt.getName() << "..." << std::endl;
      d_register[tn] = TypeNode::fromType( dt.getSygusType() );
      if( d_register[tn].isNull() ){
        Trace("sygus-split") << "...not sygus." << std::endl;
      }else{
        d_util->registerSygusType( tn );

        //compute the redundant operators
        for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
          bool nred = true;
          if( options::sygusNormalForm() ){
            Trace("sygus-split-debug") << "Is " << dt[i].getName() << " a redundant operator?" << std::endl;
            std::map< int, Kind >::iterator it = d_util->d_arg_kind[tn].find( i );
            if( it!=d_util->d_arg_kind[tn].end() ){
              Kind dk;
              if( d_util->isAntisymmetric( it->second, dk ) ){
                int j = d_util->getKindArg( tn, dk );
                if( j!=-1 ){
                  Trace("sygus-split-debug") << "Possible redundant operator : " << it->second << " with " << dk << std::endl;
                  //check for type mismatches
                  bool success = true;
                  for( unsigned k=0; k<2; k++ ){
                    unsigned ko = k==0 ? 1 : 0;
                    TypeNode tni = TypeNode::fromType( ((SelectorType)dt[i][k].getType()).getRangeType() );
                    TypeNode tnj = TypeNode::fromType( ((SelectorType)dt[j][ko].getType()).getRangeType() );
                    if( tni!=tnj ){
                      Trace("sygus-split-debug") << "Argument types " << tni << " and " << tnj << " are not equal." << std::endl;
                      success = false;
                      break;
                    }
                  }
                  if( success ){
                    Trace("sygus-nf") << "* Sygus norm " << dt.getName() << " : do not consider any " << d_util->d_arg_kind[tn][i] << " terms." << std::endl;
                    nred = false;
                  }
                }
              }
            }
            if( nred ){
              Trace("sygus-split-debug") << "Check " << dt[i].getName() << " based on generic rewriting" << std::endl;
              std::map< TypeNode, int > var_count;
              std::map< int, Node > pre;
              Node g = d_util->mkGeneric( dt, i, var_count, pre );
              nred = !isGenericRedundant( tn, g );
              Trace("sygus-split-debug") << "...done check " << dt[i].getName() << " based on generic rewriting" << std::endl;
            }
          }
          d_sygus_nred[tn].push_back( nred );
        }
      }
      Trace("sygus-split-debug") << "...done register type " << dt.getName() << std::endl;
    }
  }
}

void SygusSplit::registerSygusTypeConstructorArg( TypeNode tnn, const Datatype& dt, TypeNode tnnp, const Datatype& pdt, int csIndex, int sIndex ) {
  std::map< int, std::vector< bool > >::iterator it = d_sygus_pc_nred[tnn][csIndex].find( sIndex );
  if( it==d_sygus_pc_nred[tnn][csIndex].end() ){
    d_sygus_pc_nred[tnn][csIndex][sIndex].clear();
    registerSygusType( tnn );
    registerSygusType( tnnp );
    Trace("sygus-split") << "Register type constructor arg " << dt.getName() << " " << csIndex << " " << sIndex << std::endl;
    if( !options::sygusNormalForm() ){
      for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
        d_sygus_pc_nred[tnn][csIndex][sIndex].push_back( true );
      }
    }else{
      // calculate which constructors we should consider based on normal form for terms
      //get parent kind
      Kind parentKind = d_util->getArgKind( tnnp, csIndex );
      for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
        Assert( d_sygus_nred.find( tnn )!=d_sygus_nred.end() );
        bool addSplit = d_sygus_nred[tnn][i];
        if( addSplit && parentKind!=UNDEFINED_KIND ){
          if( d_util->d_arg_kind.find( tnn )!=d_util->d_arg_kind.end() && d_util->d_arg_kind[tnn].find( i )!=d_util->d_arg_kind[tnn].end() ){
            addSplit = considerSygusSplitKind( dt, pdt, tnn, tnnp, d_util->d_arg_kind[tnn][i], parentKind, sIndex );
            if( !addSplit ){
              Trace("sygus-nf") << "* Sygus norm " << pdt.getName() << " : do not consider " << dt.getName() << "::" << d_util->d_arg_kind[tnn][i];
              Trace("sygus-nf") << " as argument " << sIndex << " of " << parentKind << "." << std::endl;
            }
          }else if( d_util->d_arg_const.find( tnn )!=d_util->d_arg_const.end() && d_util->d_arg_const[tnn].find( i )!=d_util->d_arg_const[tnn].end() ){
            addSplit = considerSygusSplitConst( dt, pdt, tnn, tnnp, d_util->d_arg_const[tnn][i], parentKind, sIndex );
            if( !addSplit ){
              Trace("sygus-nf") << "* Sygus norm " << pdt.getName() << " : do not consider constant " << dt.getName() << "::" << d_util->d_arg_const[tnn][i];
              Trace("sygus-nf") << " as argument " << sIndex << " of " << parentKind << "." << std::endl;
            }
          }
          if( addSplit ){
            if( pdt[csIndex].getNumArgs()==1 ){
              //generic rewriting
              std::map< int, Node > prec;
              std::map< TypeNode, int > var_count;
              Node gc = d_util->mkGeneric( dt, i, var_count, prec );
              std::map< int, Node > pre;
              pre[sIndex] = gc;
              Node g = d_util->mkGeneric( pdt, csIndex, var_count, pre );
              addSplit = !isGenericRedundant( tnnp, g );
            }
            /*
            else{
              Trace("sygus-nf-temp") << "Check " << dt[i].getName() << " as argument " << sIndex << " under " << parentKind << std::endl;
              std::map< int, Node > prec;
              std::map< TypeNode, int > var_count;
              Node gc = d_util->mkGeneric( dt, i, var_count, prec );
              std::map< int, Node > pre;
              pre[sIndex] = gc;
              Node g = d_util->mkGeneric( pdt, csIndex, var_count, pre );
              bool tmp = !isGenericRedundant( tnnp, g, false );
            }
            */
          }
        }
        d_sygus_pc_nred[tnn][csIndex][sIndex].push_back( addSplit );
      }
      //compute argument relationships for 2-arg constructors
      if( parentKind!=UNDEFINED_KIND && pdt[csIndex].getNumArgs()==2 ){
        int osIndex = sIndex==0 ? 1 : 0;
        TypeNode tnno = getArgType( pdt[csIndex], osIndex );
        if( DatatypesRewriter::isTypeDatatype( tnno ) ){
          const Datatype& dto = ((DatatypeType)(tnno).toType()).getDatatype();
          registerSygusTypeConstructorArg( tnno, dto, tnnp, pdt, csIndex, osIndex );
          //compute relationships when doing 0-arg
          if( sIndex==0 ){
            Assert( d_sygus_pc_nred[tnn][csIndex].find( sIndex )!=d_sygus_pc_nred[tnn][csIndex].end() );
            Assert( d_sygus_pc_nred[tnno][csIndex].find( osIndex )!=d_sygus_pc_nred[tnno][csIndex].end() );

            bool isPComm = d_util->isComm( parentKind );
            std::map< int, bool > larg_consider;
            for( unsigned i=0; i<dto.getNumConstructors(); i++ ){
              if( d_sygus_pc_nred[tnno][csIndex][osIndex][i] ){
                //arguments that can be removed
                std::map< int, bool > rem_arg;
                //collect information about this index
                bool isSingularConst = d_util->isConstArg( tnno, i ) && d_util->isSingularArg( d_util->d_arg_const[tnno][i], parentKind, 1 );
                bool argConsider = false;
                for( unsigned j=0; j<dt.getNumConstructors(); j++ ){
                  if( d_sygus_pc_nred[tnn][csIndex][sIndex][j] ){
                    Trace("sygus-split-debug") << "Check redundancy of " << dt[j].getSygusOp() << " and " << dto[i].getSygusOp() << " under " << parentKind << std::endl;
                    bool rem = false;
                    if( isPComm && j>i && tnn==tnno && d_sygus_pc_nred[tnno][csIndex][osIndex][j] ){
                      //based on commutativity
                      // use term ordering : constructor index of first argument is not greater than constructor index of second argument
                      rem = true;
                      Trace("sygus-nf") << "* Sygus norm : commutativity of " << parentKind << " : consider " << dt[j].getSygusOp() << " before " << dto[i].getSygusOp() << std::endl;
                    }else if( isSingularConst && argConsider ){
                      rem = true;
                      Trace("sygus-nf") << "* Sygus norm : RHS singularity arg " << dto[i].getSygusOp() << " of " << parentKind;
                      Trace("sygus-nf") << " : do not consider " << dt[j].getSygusOp() << " as first arg." << std::endl;
                    }else if( d_util->isConstArg( tnn, j ) && d_util->isSingularArg( d_util->d_arg_const[tnn][j], parentKind, 0 ) && larg_consider.find( j )!=larg_consider.end() ){
                      rem = true;
                      Trace("sygus-nf") << "* Sygus norm : LHS singularity arg " << dt[j].getSygusOp() << " of " << parentKind;
                      Trace("sygus-nf") << " : do not consider " << dto[i].getSygusOp() << " as second arg." << std::endl;
                    }else{
                      if( parentKind!=UNDEFINED_KIND ){
                        std::map< TypeNode, int > var_count;
                        std::map< int, Node > pre;
                        Node g1 = d_util->mkGeneric( dt, j, var_count, pre );
                        Node g2 = d_util->mkGeneric( dto, i, var_count, pre );
                        Node g = NodeManager::currentNM()->mkNode( parentKind, g1, g2 );
                        if( isGenericRedundant( tnnp, g ) ){
                          rem = true;
                        }
                      }
                    }
                    if( rem ){
                      rem_arg[j] = true;
                    }else{
                      argConsider = true;
                      larg_consider[j] = true;
                    }
                  }
                }
                if( !rem_arg.empty() ){
                  d_sygus_pc_arg_pos[tnno][csIndex][i].clear();
                  Trace("sygus-split-debug") << "Possibilities for first argument of " << parentKind << ", when second argument is " << dto[i].getName() << " : " << std::endl;
                  for( unsigned j=0; j<dt.getNumConstructors(); j++ ){
                    if( d_sygus_pc_nred[tnn][csIndex][sIndex][j] && rem_arg.find( j )==rem_arg.end() ){
                      d_sygus_pc_arg_pos[tnno][csIndex][i].push_back( j );
                      Trace("sygus-split-debug") << "  " << dt[j].getName() << std::endl;
                    }
                  }
                  //if there are no possibilities for first argument, then this child is redundant
                  if( d_sygus_pc_arg_pos[tnno][csIndex][i].empty() ){
                    Trace("sygus-nf") << "* Sygus norm " << pdt.getName() << " : do not consider constant " << dt.getName() << "::" << dto[i].getName();
                    Trace("sygus-nf") << " as argument " << osIndex << " of " << parentKind << " (based on arguments)." << std::endl;
                    d_sygus_pc_nred[tnno][csIndex][osIndex][i] = false;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

//this function gets all easy redundant cases, before consulting rewriters
bool SygusSplit::considerSygusSplitKind( const Datatype& dt, const Datatype& pdt, TypeNode tn, TypeNode tnp, Kind k, Kind parent, int arg ) {
  Assert( d_util->hasKind( tn, k ) );
  Assert( d_util->hasKind( tnp, parent ) );
  Trace("sygus-split") << "Consider sygus split kind " << k << ", parent = " << parent << ", arg = " << arg << "?" << std::endl;
  int c = d_util->getKindArg( tn, k );
  int pc = d_util->getKindArg( tnp, parent );
  if( k==parent ){
    //check for associativity
    if( d_util->isAssoc( k ) ){
      //if the operator is associative, then a repeated occurrence should only occur in the leftmost argument position
      int firstArg = getFirstArgOccurrence( pdt[pc], dt );
      Assert( firstArg!=-1 );
      Trace("sygus-split-debug") << "Associative, with first arg = " << firstArg << std::endl;
      return arg==firstArg;
    }
  }
  //push
  if( parent==NOT || parent==BITVECTOR_NOT || parent==UMINUS || parent==BITVECTOR_NEG ){
     //negation normal form
    if( parent==k && isArgDatatype( dt[c], 0, pdt ) ){
      return false;
    }
    Kind nk = UNDEFINED_KIND;
    Kind reqk = UNDEFINED_KIND;  //required kind for all children
    if( parent==NOT ){
      if( k==AND ) {
        nk = OR;reqk = NOT;
      }else if( k==OR ){
        nk = AND;reqk = NOT;
      }else if( k==IFF ) {
        nk = XOR;
      }else if( k==XOR ) {
        nk = IFF;
      }
    }
    if( parent==BITVECTOR_NOT ){
      if( k==BITVECTOR_AND ) {
        nk = BITVECTOR_OR;reqk = BITVECTOR_NOT;
      }else if( k==BITVECTOR_OR ){
        nk = BITVECTOR_AND;reqk = BITVECTOR_NOT;
      }else if( k==BITVECTOR_XNOR ) {
        nk = BITVECTOR_XOR;
      }else if( k==BITVECTOR_XOR ) {
        nk = BITVECTOR_XNOR;
      }
    }
    if( parent==UMINUS ){
      if( k==PLUS ){
        nk = PLUS;reqk = UMINUS;
      }
    }
    if( parent==BITVECTOR_NEG ){
      if( k==PLUS ){
        nk = PLUS;reqk = BITVECTOR_NEG;
      }
    }
    if( nk!=UNDEFINED_KIND ){
      Trace("sygus-split-debug") << "Push " << parent << " over " << k << " to " << nk;
      if( reqk!=UNDEFINED_KIND ){
        Trace("sygus-split-debug") << ", reqk = " << reqk;
      }
      Trace("sygus-split-debug") << "?" << std::endl;
      int pcr = d_util->getKindArg( tnp, nk );
      if( pcr!=-1 ){
        Assert( pcr<(int)pdt.getNumConstructors() );
        if( reqk!=UNDEFINED_KIND ){
          //must have same number of arguments
          if( pdt[pcr].getNumArgs()==dt[c].getNumArgs() ){
            bool success = true;
            std::map< int, TypeNode > childTypes;
            for( unsigned i=0; i<pdt[pcr].getNumArgs(); i++ ){
              TypeNode tna = getArgType( pdt[pcr], i );
              Assert( datatypes::DatatypesRewriter::isTypeDatatype( tna ) );
              if( reqk!=UNDEFINED_KIND ){
                //child must have a NOT
                int nindex = d_util->getKindArg( tna, reqk );
                if( nindex!=-1 ){
                  const Datatype& adt = ((DatatypeType)(tn).toType()).getDatatype();
                  if( getArgType( dt[c], i )!=getArgType( adt[nindex], 0 ) ){
                    Trace("sygus-split-debug") << "...arg " << i << " type mismatch." << std::endl;
                    success = false;
                    break;
                  }
                }else{
                  Trace("sygus-split-debug") << "...argument " << i << " does not have " << reqk << "." << std::endl;
                  success = false;
                  break;
                }
              }else{
                childTypes[i] = tna;
              }
            }
            if( success ){
              Trace("sygus-split-debug") << "...success" << std::endl;
              return false;
            }
          }else{
            Trace("sygus-split-debug") << "...#arg mismatch." << std::endl;
          }
        }else{
          return !isTypeMatch( pdt[pcr], dt[c] );
        }
      }else{
        Trace("sygus-split-debug") << "...operator not available." << std::endl;
      }
    }
  }
  if( parent==MINUS || parent==BITVECTOR_SUB ){
    
    
  }
  return true;
}

//this function gets all easy redundant cases, before consulting rewriters
bool SygusSplit::considerSygusSplitConst( const Datatype& dt, const Datatype& pdt, TypeNode tn, TypeNode tnp, Node c, Kind parent, int arg ) {
  Assert( d_util->hasConst( tn, c ) );
  Assert( d_util->hasKind( tnp, parent ) );
  int pc = d_util->getKindArg( tnp, parent );
  Trace("sygus-split") << "Consider sygus split const " << c << ", parent = " << parent << ", arg = " << arg << "?" << std::endl;
  if( d_util->isIdempotentArg( c, parent, arg ) ){
    Trace("sygus-split-debug") << "  " << c << " is idempotent arg " << arg << " of " << parent << "..." << std::endl;
    if( pdt[pc].getNumArgs()==2 ){
      int oarg = arg==0 ? 1 : 0;
      TypeNode otn = TypeNode::fromType( ((SelectorType)pdt[pc][oarg].getType()).getRangeType() );
      if( otn==tnp ){
        return false;
      }
    }
  }else if( d_util->isSingularArg( c, parent, arg ) ){
    Trace("sygus-split-debug") << "  " << c << " is singular arg " << arg << " of " << parent << "..." << std::endl;
    if( d_util->hasConst( tnp, c ) ){
      return false;
    }
  }
  if( pdt[pc].getNumArgs()==2 ){
    Kind ok;
    int offset;
    if( d_util->hasOffsetArg( parent, arg, offset, ok ) ){
      Trace("sygus-split-debug") << parent << " has offset arg " << ok << " " << offset << std::endl;
      int ok_arg = d_util->getKindArg( tnp, ok );
      if( ok_arg!=-1 ){
        Trace("sygus-split-debug") << "...at argument " << ok_arg << std::endl;
        //other operator be the same type
        if( isTypeMatch( pdt[ok_arg], pdt[arg] ) ){
          int status;
          Node co = d_util->getTypeValueOffset( c.getType(), c, offset, status );
          Trace("sygus-split-debug") << c << " with offset " << offset << " is " << co << ", status=" << status << std::endl;
          if( status==0 && !co.isNull() ){
            if( d_util->hasConst( tn, co ) ){
              Trace("sygus-split-debug") << "arg " << arg << " " << c << " in " << parent << " can be treated as " << co << " in " << ok << "..." << std::endl;
              return false;
            }else{
              Trace("sygus-split-debug") << "Type does not have constant." << std::endl;
            }
          }
        }else{
          Trace("sygus-split-debug") << "Type mismatch." << std::endl;
        }
      }
    }
  }
  return true;
}

int SygusSplit::getFirstArgOccurrence( const DatatypeConstructor& c, const Datatype& dt ) {
  for( unsigned i=0; i<c.getNumArgs(); i++ ){
    if( isArgDatatype( c, i, dt ) ){
      return i;
    }
  }
  return -1;
}

bool SygusSplit::isArgDatatype( const DatatypeConstructor& c, int i, const Datatype& dt ) {
  TypeNode tni = getArgType( c, i );
  if( datatypes::DatatypesRewriter::isTypeDatatype( tni ) ){
    const Datatype& adt = ((DatatypeType)(tni).toType()).getDatatype();
    if( adt==dt ){
      return true;
    }
  }
  return false;
}

TypeNode SygusSplit::getArgType( const DatatypeConstructor& c, int i ) {
  Assert( i>=0 && i<(int)c.getNumArgs() );
  return TypeNode::fromType( ((SelectorType)c[i].getType()).getRangeType() );
}

bool SygusSplit::isTypeMatch( const DatatypeConstructor& c1, const DatatypeConstructor& c2 ){
  if( c1.getNumArgs()!=c2.getNumArgs() ){
    return false;
  }else{
    for( unsigned i=0; i<c1.getNumArgs(); i++ ){
      if( getArgType( c1, i )!=getArgType( c2, i ) ){
        return false;
      }
    }
    return true;
  }
}

bool SygusSplit::isGenericRedundant( TypeNode tn, Node g, bool active ) {
  //everything added to this cache should be mutually exclusive cases
  std::map< Node, bool >::iterator it = d_gen_redundant[tn].find( g );
  if( it==d_gen_redundant[tn].end() ){
    Trace("sygus-gnf") << "Register generic for " << tn << " : " << g << std::endl;
    Node gr = d_util->getNormalized( tn, g, false );
    Trace("sygus-gnf-debug") << "Generic " << g << " rewrites to " << gr << std::endl;
    if( active ){
      std::map< Node, Node >::iterator itg = d_gen_terms[tn].find( gr );
      bool red = true;
      if( itg==d_gen_terms[tn].end() ){
        red = false;
        d_gen_terms[tn][gr] = g;
        d_gen_terms_inactive[tn][gr] = g;
        Trace("sygus-gnf-debug") << "...not redundant." << std::endl;
      }else{
        Trace("sygus-gnf-debug") << "...redundant." << std::endl;
        Trace("sygus-nf") << "* Sygus normal form : simplify since " << g << " and " << itg->second << " both rewrite to " << gr << std::endl;
      }
      d_gen_redundant[tn][g] = red;
      return red;
    }else{
      std::map< Node, Node >::iterator itg = d_gen_terms_inactive[tn].find( gr );
      if( itg==d_gen_terms_inactive[tn].end() ){
        Trace("sygus-nf-temp") << "..." << g << " rewrites to " << gr << std::endl;
        d_gen_terms_inactive[tn][gr] = g;
      }else{
        Trace("sygus-nf-temp") << "* Note " << g << " and " << itg->second << " both rewrite to " << gr << std::endl;
      }
      return false;
    }
  }else{
    return it->second;
  }
}



SygusSymBreak::SygusSymBreak( SygusUtil * util, context::Context* c ) :
d_util( util ), d_context( c ) {

}

void SygusSymBreak::addTester( Node tst ) {
  if( options::sygusNormalFormGlobal() ){
    Node a = getAnchor( tst[0] );
    Trace("sygus-sym-break-debug") << "Add tester " << tst << " for " << a << std::endl;
    std::map< Node, ProgSearch * >::iterator it = d_prog_search.find( a );
    ProgSearch * ps;
    if( it==d_prog_search.end() ){
      ps = new ProgSearch( this, a, d_context );
      d_prog_search[a] = ps;
    }else{
      ps = it->second;
    }
    ps->addTester( tst );
  }
}

Node SygusSymBreak::getAnchor( Node n ) {
  if( n.getKind()==APPLY_SELECTOR_TOTAL ){
    return getAnchor( n[0] );
  }else{
    return n;
  }
}

void SygusSymBreak::ProgSearch::addTester( Node tst ) {
  NodeMap::const_iterator it = d_testers.find( tst[0] );
  if( it==d_testers.end() ){
    d_testers[tst[0]] = tst;
    if( tst[0]==d_anchor ){
      assignTester( tst, 0 );
    }else{
      IntMap::const_iterator it = d_watched_terms.find( tst[0] );
      if( it!=d_watched_terms.end() ){
        assignTester( tst, (*it).second );
      }else{
        Trace("sygus-sym-break-debug2") << "...add to wait list " << tst << " for " << d_anchor << std::endl;
      }
    }
  }else{
    Trace("sygus-sym-break-debug2") << "...already seen " << tst << " for " << d_anchor << std::endl;
  }
}

bool SygusSymBreak::ProgSearch::assignTester( Node tst, int depth ) {
  Trace("sygus-sym-break-debug") << "SymBreak : Assign tester : " << tst << ", depth = " << depth << " of " << d_anchor << std::endl;
  int tindex = Datatype::indexOf( tst.getOperator().toExpr() );
  TypeNode tn = tst[0].getType();
  Assert( DatatypesRewriter::isTypeDatatype( tn ) );
  const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
  std::vector< Node > tst_waiting;
  for( unsigned i=0; i<dt[tindex].getNumArgs(); i++ ){
    Node sel = NodeManager::currentNM()->mkNode( kind::APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[tindex][i].getSelector() ), tst[0] );
    NodeMap::const_iterator it = d_testers.find( sel );
    if( it!=d_testers.end() ){
      tst_waiting.push_back( (*it).second );
    }else{
      Trace("sygus-sym-break-debug") << "...add " << sel << " as watch term for " << (depth+1) << std::endl;
      d_watched_terms[sel] = depth+1;
    }
  }
  //update watched count
  IntIntMap::const_iterator it = d_watched_count.find( depth+1 );
  if( it==d_watched_count.end() ){
    d_watched_count[depth+1] = dt[tindex].getNumArgs();
  }else{
    d_watched_count[depth+1] = d_watched_count[depth+1] + dt[tindex].getNumArgs();
  }
  Trace("sygus-sym-break-debug") << "...watched count now " << d_watched_count[depth+1].get() << " for " << (depth+1) << " of " << d_anchor << std::endl;
  //now decrement watch count and process
  if( depth>0 ){
    Assert( d_watched_count[depth]>0 );
    d_watched_count[depth] = d_watched_count[depth] - 1;
  }
  //determine if any subprograms on the current path are redundant
  if( processSubprograms( tst[0], depth, depth ) ){
    if( processProgramDepth( depth ) ){
      //assign preexisting testers
      for( unsigned i=0; i<tst_waiting.size(); i++ ){
        if( !assignTester( tst_waiting[i], depth+1 ) ){
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool SygusSymBreak::ProgSearch::processProgramDepth( int depth ){
  if( depth==d_prog_depth.get() && ( depth==0 || ( d_watched_count.find( depth )!=d_watched_count.end() && d_watched_count[depth]==0 ) ) ){
    d_prog_depth = d_prog_depth + 1;
    if( depth>0 ){
      Trace("sygus-sym-break-debug") << "Program is set for depth " << depth << std::endl;
      std::map< TypeNode, int > var_count;
      std::vector< Node > testers;
      std::map< Node, std::vector< Node > > testers_u;
      //now have entire information about candidate program at given depth
      Node prog = getCandidateProgramAtDepth( depth, d_anchor, 0, Node::null(), var_count, testers, testers_u );
      if( !prog.isNull() ){
        if( !d_parent->processCurrentProgram( d_anchor, d_anchor_type, depth, prog, testers, testers_u, var_count ) ){
          return false;
        }
      }else{
        Assert( false );
      }
    }
    return processProgramDepth( depth+1 );
  }else{
    return true;
  }
}

bool SygusSymBreak::ProgSearch::processSubprograms( Node n, int depth, int odepth ) {
  Trace("sygus-sym-break-debug") << "Processing subprograms on path " << n << ", which has depth " << depth << std::endl;
  depth--;
  if( depth>0 ){
    Assert( n.getKind()==APPLY_SELECTOR_TOTAL );
    std::map< TypeNode, int > var_count;
    std::vector< Node > testers;
    std::map< Node, std::vector< Node > > testers_u;
    //now have entire information about candidate program at given depth
    Node prog = getCandidateProgramAtDepth( odepth-depth, n[0], 0, Node::null(), var_count, testers, testers_u );
    if( !prog.isNull() ){
      if( !d_parent->processCurrentProgram( n[0], n[0].getType(), odepth-depth, prog, testers, testers_u, var_count ) ){
        return false;
      }
      //also try higher levels
      return processSubprograms( n[0], depth, odepth );
    }else{
      Trace("sygus-sym-break-debug") << "...program incomplete." << std::endl;
    }
  }
  return true;
}

Node SygusSymBreak::ProgSearch::getCandidateProgramAtDepth( int depth, Node prog, int curr_depth, Node parent, std::map< TypeNode, int >& var_count,
                                                            std::vector< Node >& testers, std::map< Node, std::vector< Node > >& testers_u ) {
  Assert( depth>=curr_depth );
  Trace("sygus-sym-break-debug") << "Reconstructing program for " << prog << " at depth " << curr_depth << "/" << depth << std::endl;
  NodeMap::const_iterator it = d_testers.find( prog );
  if( it!=d_testers.end() ){
    Node tst = (*it).second;
    testers.push_back( tst );
    testers_u[parent].push_back( tst );
    Assert( tst[0]==prog );
    int tindex = Datatype::indexOf( tst.getOperator().toExpr() );
    TypeNode tn = prog.getType();
    Assert( DatatypesRewriter::isTypeDatatype( tn ) );
    const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
    std::map< int, Node > pre;
    if( curr_depth<depth ){
      for( unsigned i=0; i<dt[tindex].getNumArgs(); i++ ){
        Node sel = NodeManager::currentNM()->mkNode( kind::APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[tindex][i].getSelector() ), prog );
        pre[i] = getCandidateProgramAtDepth( depth, sel, curr_depth+1, prog, var_count, testers, testers_u );
        if( pre[i].isNull() ){
          return Node::null();
        }
      }
    }
    return d_parent->d_util->mkGeneric( dt, tindex, var_count, pre );
  }else{
    Trace("sygus-sym-break-debug") << "...failure." << std::endl;
    return Node::null();
  }
}

bool SygusSymBreak::processCurrentProgram( Node a, TypeNode at, int depth, Node prog,
                                           std::vector< Node >& testers, std::map< Node, std::vector< Node > >& testers_u,
                                           std::map< TypeNode, int >& var_count ) {
  Assert( a.getType()==at );
  //Assert( !d_util->d_conflict );
  std::map< Node, bool >::iterator it = d_redundant[at].find( prog );
  bool red;
  if( it==d_redundant[at].end() ){
    Trace("sygus-sym-break") << "Currently considering program : " << prog << " at depth " << depth << " for " << a << std::endl;
    Node progr = d_util->getNormalized( at, prog );
    Node rep_prog;
    std::map< Node, Node >::iterator itnp = d_normalized_to_orig[at].find( progr );
    int tsize = d_util->getTermSize( prog );
    if( itnp==d_normalized_to_orig[at].end() ){
      d_normalized_to_orig[at][progr] = prog;
      if( progr.getKind()==SKOLEM && d_util->getSygusType( progr )==at ){
        Trace("sygus-nf") << "* Sygus sym break : " << prog << " rewrites to variable " << progr << " of same type as self" << std::endl;
        d_redundant[at][prog] = true;
        red = true;
      }else{
        d_redundant[at][prog] = false;
        red = false;
      }
    }else{
      rep_prog = itnp->second;
      if( tsize<d_normalized_to_term_size[at][progr] ){
        d_normalized_to_orig[at][progr] = prog;
        Trace("sygus-nf-debug") << "Program is redundant, but has smaller size than " << rep_prog << std::endl;
        d_redundant[at].erase( rep_prog );
        d_redundant[at][prog] = false;
        red = false;
      }else{
        Assert( prog!=itnp->second );
        d_redundant[at][prog] = true;
        red = true;
        Trace("sygus-nf") << "* Sygus sym break : " << prog << " and " << rep_prog << " both rewrite to " << progr << std::endl;
        Trace("sygus-nf-debug") << "  sizes : " << tsize << " " << d_normalized_to_term_size[at][progr] << std::endl;
      }
    }
    if( !red ){
      d_normalized_to_term_size[at][progr] = tsize;
    }else{
      Assert( !testers.empty() );
      bool conflict_gen_set = false;
      if( options::sygusNormalFormGlobalGen() ){
        bool narrow = false;
        Trace("sygus-nf-gen-debug") << "Tester tree is : " << std::endl;
        for( std::map< Node, std::vector< Node > >::iterator it = testers_u.begin(); it != testers_u.end(); ++it ){
          Trace("sygus-nf-gen-debug") << "  " << it->first << " -> " << std::endl;
          for( unsigned i=0; i<it->second.size(); i++ ){
            Trace("sygus-nf-gen-debug") << "    " << it->second[i] << std::endl;
          }
        }
        Trace("sygus-nf-gen-debug") << std::endl;

        //generalize conflict
        if( prog.getNumChildren()>0 ){
          Assert( !testers.empty() );
          d_util->registerSygusType( at );
          //Trace("sygus-nf-gen-debug") << "Testers are : " << std::endl;
          //for( unsigned i=0; i<testers.size(); i++ ){
          //  Trace("sygus-nf-gen-debug") << "* " << testers[i] << std::endl;
          //}
          Assert( testers[0][0]==a );
          Assert( prog.getNumChildren()==testers_u[a].size() );
          //get the normal form for each child
          Kind parentKind = prog.getKind();
          Kind parentOpKind = prog.getOperator().getKind();
          Trace("sygus-nf-gen-debug") << "Parent kind is " << parentKind << " " << parentOpKind << std::endl;
          //std::map< int, Node > norm_children;

          //arguments that are relevant
          std::map< unsigned, bool > rlv;
          //testers that are irrelevant
          std::map< Node, bool > irrlv_tst;

          std::vector< Node > children;
          std::vector< TypeNode > children_stype;
          std::vector< Node > nchildren;
          for( unsigned i=0; i<testers_u[a].size(); i++ ){
            TypeNode tn = testers_u[a][i][0].getType();
            children.push_back( prog[i] );
            children_stype.push_back( tn );
            Node nc = d_util->getNormalized( tn, prog[i], true );
            //norm_children[i] = nc;
            rlv[i] = true;
            nchildren.push_back( nc );
            Trace("sygus-nf-gen") << "- child " << i << " normalizes to " << nc << std::endl;
          }
          if( testers_u[a].size()>1 ){
            bool finished = false;
            const Datatype & pdt = ((DatatypeType)(at).toType()).getDatatype();
            int pc = Datatype::indexOf( testers[0].getOperator().toExpr() );
            // [1] determine a minimal subset of the arguments that the rewriting depended on
            //quick checks based on constants
            for( unsigned i=0; i<nchildren.size(); i++ ){
              Node arg = nchildren[i];
              if( arg.isConst() ){
                if( parentOpKind==kind::BUILTIN ){
                  Trace("sygus-nf-gen") << "-- constant arg " <<i << " under builtin operator." << std::endl;
                  if( !processConstantArg( at, pdt, pc, parentKind, i, arg, rlv ) ){
                    Trace("sygus-nf") << "  - argument " << i << " is singularly redundant." << std::endl;
                    for( std::map< unsigned, bool >::iterator itr = rlv.begin(); itr != rlv.end(); ++itr ){
                      if( itr->first!=i ){
                        rlv[itr->first] = false;
                      }
                    }
                    narrow = true;
                    finished = true;
                    break;
                  }
                }
              }
            }

            if( !finished ){
              // [2] check replacing each argument with a fresh variable gives the same result
              Node progc = prog;
              if( options::sygusNormalFormGlobalArg() ){
                bool argChanged = false;
                for( unsigned i=0; i<prog.getNumChildren(); i++ ){
                  Node prev = children[i];
                  children[i] = d_util->getVarInc( children_stype[i], var_count );
                  Node progcn = NodeManager::currentNM()->mkNode( prog.getKind(), children );
                  Node progcr = Rewriter::rewrite( progcn );
                  Trace("sygus-nf-gen-debug") << "Var replace argument " << i << " : " << progcn << " -> " << progcr << std::endl;
                  if( progcr==progr ){
                    //this argument is not relevant, continue with it remaining as variable
                    rlv[i] = false;
                    argChanged = true;
                    narrow = true;
                    Trace("sygus-nf") << "  - argument " << i << " is not relevant." << std::endl;
                  }else{
                    //go back to original
                    children[i] = prev;
                    var_count[children_stype[i]]--;
                  }
                }
                if( argChanged ){
                  progc = NodeManager::currentNM()->mkNode( prog.getKind(), children );
                }
              }
              Trace("sygus-nf-gen-debug") << "Relevant template (post argument analysis) is : " << progc << std::endl;

              // [3] generalize content
              if( options::sygusNormalFormGlobalContent() ){
                std::map< Node, std::vector< Node > > nodes;
                std::vector< Node > curr_vars;
                std::vector< Node > curr_subs;
                collectSubterms( progc, testers[0], testers_u, nodes );
                for( std::map< Node, std::vector< Node > >::iterator it = nodes.begin(); it != nodes.end(); ++it ){
                  if( it->second.size()>1 ){
                    Trace("sygus-nf-gen-debug") << it->first << " occurs " << it->second.size() << " times, at : " << std::endl;
                    bool success = true;
                    TypeNode tn;
                    for( unsigned j=0; j<it->second.size(); j++ ){
                      Trace("sygus-nf-gen-debug") << "  " << it->second[j] << " ";
                      TypeNode tnc = it->second[j][0].getType();
                      if( !tn.isNull() && tn!=tnc ){
                        success = false;
                      }
                      tn = tnc;
                    }
                    Trace("sygus-nf-gen-debug") << std::endl;
                    if( success ){
                      Node prev = progc;
                      //try a substitution on all terms of this form simultaneously to see if the content of this subterm is irrelevant
                      TypeNode tn = it->second[0][0].getType();
                      TNode st = it->first;
                      //we may already have substituted within this subterm
                      if( !curr_subs.empty() ){
                        st = st.substitute( curr_vars.begin(), curr_vars.end(), curr_subs.begin(), curr_subs.end() );
                        Trace("sygus-nf-gen-debug") << "...substituted : " << st << std::endl;
                      }
                      TNode nv = d_util->getVarInc( tn, var_count );
                      progc = progc.substitute( st, nv );
                      Node progcr = Rewriter::rewrite( progc );
                      Trace("sygus-nf-gen-debug") << "Var replace content " << st << " : " << progc << " -> " << progcr << std::endl;
                      if( progcr==progr ){
                        narrow = true;
                        Trace("sygus-nf") << "  - content " << st << " is not relevant." << std::endl;
                        int t_prev = -1;
                        for( unsigned i=0; i<it->second.size(); i++ ){
                          irrlv_tst[it->second[i]] = true;
                          Trace("sygus-nf-gen-debug") << "By content, " << it->second[i] << " is irrelevant." << std::endl;
                          int t_curr = std::find( testers.begin(), testers.end(), it->second[i] )-testers.begin();
                          Assert( testers[t_curr]==it->second[i] );
                          if( t_prev!=-1 ){
                            d_lemma_inc_eq[at][prog].push_back( std::pair< int, int >( t_prev, t_curr ) );
                            Trace("sygus-nf-gen-debug") << "Which requires " << testers[t_prev][0] << " = " << testers[t_curr][0] << std::endl;
                          }
                          t_prev = t_curr;
                        }
                        curr_vars.push_back( st );
                        curr_subs.push_back( nv );
                      }else{
                        var_count[tn]--;
                        progc = prev;
                      }
                    }else{
                      Trace("sygus-nf-gen-debug") << "...content is from multiple grammars, abort." << std::endl;
                    }
                  }
                }
              }
              Trace("sygus-nf-gen-debug") << "Relevant template (post content analysis) is : " << progc << std::endl;
            }
            if( narrow ){
              //relevant testers : root + recursive collection of relevant children
              Trace("sygus-nf-gen-debug") << "Collect relevant testers..." << std::endl;
              std::vector< Node > rlv_testers;
              rlv_testers.push_back( testers[0] );
              for( unsigned i=0; i<testers_u[a].size(); i++ ){
                if( rlv[i] ){
                  collectTesters( testers_u[a][i], testers_u, rlv_testers, irrlv_tst );
                }
              }
              //must guard case : generalized lemma cannot exclude original representation
              if( !isSeparation( rep_prog, testers[0], testers_u, rlv_testers ) ){
                //must construct template
                Node anc_var;
                std::map< TypeNode, Node >::iterator itav = d_anchor_var.find( at );
                if( itav==d_anchor_var.end() ){
                  anc_var = NodeManager::currentNM()->mkSkolem( "a", at, "Sygus nf global gen anchor var" );
                  d_anchor_var[at] = anc_var;
                }else{
                  anc_var = itav->second;
                }
                int status = 0;
                Node anc_temp = getSeparationTemplate( at, rep_prog, anc_var, status );
                Trace("sygus-nf") << "  -- separation template is " << anc_temp << ", status = " << status << std::endl;
                d_lemma_inc_eq_gr[status][at][prog].push_back( anc_temp );
              }else{
                Trace("sygus-nf") << "  -- no separation necessary" << std::endl;
              }
              Trace("sygus-nf-gen-debug") << "Relevant testers : " << std::endl;
              for( unsigned i=0; i<testers.size(); i++ ){
                bool rl = std::find( rlv_testers.begin(), rlv_testers.end(), testers[i] )!=rlv_testers.end();
                Trace("sygus-nf-gen-debug") << "* " << testers[i] << " -> " << rl << std::endl;
                d_lemma_inc_tst[at][prog].push_back( rl );
              }

              conflict_gen_set = true;
            }
          }
        }
      }
      if( !conflict_gen_set ){
        for( unsigned i=0; i<testers.size(); i++ ){
          d_lemma_inc_tst[at][prog].push_back( true );
        }
      }
    }
  }else{
    red = it->second;
  }
  if( red ){
    if( std::find( d_lemmas_reported[at][prog].begin(), d_lemmas_reported[at][prog].end(), a )==d_lemmas_reported[at][prog].end() ){
      d_lemmas_reported[at][prog].push_back( a );
      Assert( d_lemma_inc_tst[at][prog].size()==testers.size() );
      std::vector< Node > disj;
      //get the guard equalities
      for( unsigned r=0; r<2; r++ ){
        for( unsigned i=0; i<d_lemma_inc_eq_gr[r][at][prog].size(); i++ ){
          TNode n2 = d_lemma_inc_eq_gr[r][at][prog][i];
          if( r==1 ){
            TNode anc_var = d_anchor_var[at];
            TNode anc = a;
            Assert( !anc_var.isNull() );
            n2 = n2.substitute( anc_var, anc );
          }
          disj.push_back( a.eqNode( n2 ) );
        }
      }
      //get the equalities that should be included
      for( unsigned i=0; i<d_lemma_inc_eq[at][prog].size(); i++ ){
        TNode n1 = testers[ d_lemma_inc_eq[at][prog][i].first ][0];
        TNode n2 = testers[ d_lemma_inc_eq[at][prog][i].second ][0];
        disj.push_back( n1.eqNode( n2 ).negate() );
      }
      //get the testers that should be included
      for( unsigned i=0; i<testers.size(); i++ ){
        if( d_lemma_inc_tst[at][prog][i] ){
          disj.push_back( testers[i].negate() );
        }
      }
      Node lem = disj.size()==1 ? disj[0] : NodeManager::currentNM()->mkNode( OR, disj );
      d_util->d_lemmas.push_back( lem );
      Trace("sygus-sym-break-lemma") << "Sym break lemma : " << lem << std::endl;
    }else{
      Trace("sygus-sym-break2") << "repeated lemma for " << prog << " from " << a << std::endl;
    }
    //for now, continue adding lemmas (since we are not forcing conflicts)
    //return false;
  }
  return true;
}

bool SygusSymBreak::isSeparation( Node rep_prog, Node tst_curr, std::map< Node, std::vector< Node > >& testers_u, std::vector< Node >& rlv_testers ) {
  Trace("sygus-nf-gen-debug") << "is separation " << rep_prog << " " << tst_curr << std::endl;
  TypeNode tn = tst_curr[0].getType();
  Node rop = rep_prog.getNumChildren()==0 ? rep_prog : rep_prog.getOperator();
  //we can continue if the tester in question is relevant
  if( std::find( rlv_testers.begin(), rlv_testers.end(), tst_curr )!=rlv_testers.end() ){
    unsigned tindex = Datatype::indexOf( tst_curr.getOperator().toExpr() );
    Node op = d_util->getArgOp( tn, tindex );
    if( op!=rop ){
      Trace("sygus-nf-gen-debug") << "mismatch, success." << std::endl;
      return true;
    }else if( !testers_u[tst_curr[0]].empty() ){
      Assert( testers_u[tst_curr[0]].size()==rep_prog.getNumChildren() );
      for( unsigned i=0; i<rep_prog.getNumChildren(); i++ ){
        if( isSeparation( rep_prog[i], testers_u[tst_curr[0]][i], testers_u, rlv_testers ) ){
          return true;
        }
      }
    }
    return false;
  }else{
    Trace("sygus-nf-gen-debug") << "not relevant, fail." << std::endl;
    return false;
  }
}

Node SygusSymBreak::getSeparationTemplate( TypeNode tn,  Node rep_prog, Node anc_var, int& status ) {
  Trace("sygus-nf-gen-debug") << "get separation template " << rep_prog << std::endl;
  const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
  if( d_util->isVar( rep_prog ) ){
    status = 1;
    return anc_var;
  }else{
    Node rop = rep_prog.getNumChildren()==0 ? rep_prog : rep_prog.getOperator();
    int rop_arg = d_util->getOpArg( tn, rop );
    Assert( rop_arg>=0 && rop_arg<(int)dt.getNumConstructors() );
    Assert( rep_prog.getNumChildren()==dt[rop_arg].getNumArgs() );

    std::vector< Node > children;
    children.push_back( Node::fromExpr( dt[rop_arg].getConstructor() ) );
    for( unsigned i=0; i<rep_prog.getNumChildren(); i++ ){
      TypeNode tna = TypeNode::fromType( ((SelectorType)dt[rop_arg][i].getType()).getRangeType() );

      int new_status = 0;
      Node arg = getSeparationTemplate( tna, rep_prog[i], anc_var, new_status );
      if( new_status==1 ){
        TNode tanc_var = anc_var;
        TNode tanc_var_subs = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[rop_arg][i].getSelector() ), anc_var );
        arg = arg.substitute( tanc_var, tanc_var_subs );
        status = 1;
      }
      children.push_back( arg );
    }
    return NodeManager::currentNM()->mkNode( APPLY_CONSTRUCTOR, children );
  }
}

bool SygusSymBreak::processConstantArg( TypeNode tnp, const Datatype & pdt, int pc,
                                        Kind k, int i, Node arg, std::map< unsigned, bool >& rlv ) {
  Assert( d_util->hasKind( tnp, k ) );
  if( k==AND || k==OR || k==IFF || k==XOR || k==IMPLIES || ( k==ITE && i==0 ) ){
    return false;
  }else if( d_util->isIdempotentArg( arg, k, i ) ){
    if( pdt[pc].getNumArgs()==2 ){
      int oi = i==0 ? 1 : 0;
      TypeNode otn = TypeNode::fromType( ((SelectorType)pdt[pc][oi].getType()).getRangeType() );
      if( otn==tnp ){
        return false;
      }
    }
  }else if( d_util->isSingularArg( arg, k, i ) ){
    if( d_util->hasConst( tnp, arg ) ){
      return false;
    }
  }
  TypeNode tn = arg.getType();
  return true;
}

void SygusSymBreak::collectTesters( Node tst, std::map< Node, std::vector< Node > >& testers_u, std::vector< Node >& testers, std::map< Node, bool >& irrlv_tst ) {
  if( irrlv_tst.find( tst )==irrlv_tst.end() ){
    testers.push_back( tst );
    std::map< Node, std::vector< Node > >::iterator it = testers_u.find( tst[0] );
    if( it!=testers_u.end() ){
      for( unsigned i=0; i<it->second.size(); i++ ){
        collectTesters( it->second[i], testers_u, testers, irrlv_tst );
      }
    }
  }
}

void SygusSymBreak::collectSubterms( Node n, Node tst_curr, std::map< Node, std::vector< Node > >& testers_u, std::map< Node, std::vector< Node > >& nodes ) {
  if( !d_util->isVar( n ) ){
    nodes[n].push_back( tst_curr );
    for( unsigned i=0; i<testers_u[tst_curr[0]].size(); i++ ){
      collectSubterms( n[i], testers_u[tst_curr[0]][i], testers_u, nodes );
    }
  }
}

SygusUtil::SygusUtil( Context* c ) {
  d_split = new SygusSplit( this );
  d_sym_break = new SygusSymBreak( this, c );
}

TNode SygusUtil::getVar( TypeNode tn, int i ) {
  while( i>=(int)d_fv[tn].size() ){
    std::stringstream ss;
    TypeNode vtn = tn;
    if( datatypes::DatatypesRewriter::isTypeDatatype( tn ) ){
      const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
      ss << "fv_" << dt.getName() << "_" << i;
      if( !dt.getSygusType().isNull() ){
        vtn = TypeNode::fromType( dt.getSygusType() );
      }
    }else{
      ss << "fv_" << tn << "_" << i;
    }
    Assert( !vtn.isNull() );
    Node v = NodeManager::currentNM()->mkSkolem( ss.str(), vtn, "for sygus normal form testing" );
    d_fv_stype[v] = tn;
    d_fv[tn].push_back( v );
  }
  return d_fv[tn][i];
}

TNode SygusUtil::getVarInc( TypeNode tn, std::map< TypeNode, int >& var_count ) {
  std::map< TypeNode, int >::iterator it = var_count.find( tn );
  if( it==var_count.end() ){
    var_count[tn] = 1;
    return getVar( tn, 0 );
  }else{
    int index = it->second;
    var_count[tn]++;
    return getVar( tn, index );
  }
}

TypeNode SygusUtil::getSygusType( Node v ) {
  Assert( d_fv_stype.find( v )!=d_fv_stype.end() );
  return d_fv_stype[v];
}

Node SygusUtil::mkGeneric( const Datatype& dt, int c, std::map< TypeNode, int >& var_count, std::map< int, Node >& pre ) {
  Assert( c>=0 && c<(int)dt.getNumConstructors() );
  Assert( dt.isSygus() );
  Assert( !dt[c].getSygusOp().isNull() );
  std::vector< Node > children;
  Node op = Node::fromExpr( dt[c].getSygusOp() );
  if( op.getKind()!=BUILTIN ){
    children.push_back( op );
  }
  for( int i=0; i<(int)dt[c].getNumArgs(); i++ ){
    TypeNode tna = TypeNode::fromType( ((SelectorType)dt[c][i].getType()).getRangeType() );
    Node a;
    std::map< int, Node >::iterator it = pre.find( i );
    if( it!=pre.end() ){
      a = it->second;
    }else{
      a = getVarInc( tna, var_count );
    }
    Assert( !a.isNull() );
    children.push_back( a );
  }
  if( op.getKind()==BUILTIN ){
    return NodeManager::currentNM()->mkNode( op, children );
  }else{
    if( children.size()==1 ){
      return children[0];
    }else{
      return NodeManager::currentNM()->mkNode( APPLY, children );
      /*
      Node n = NodeManager::currentNM()->mkNode( APPLY, children );
      //must expand definitions
      Node ne = Node::fromExpr( smt::currentSmtEngine()->expandDefinitions( n.toExpr() ) );
      Trace("sygus-util-debug") << "Expanded definitions in " << n << " to " << ne << std::endl;
      return ne;
      */
    }
  }
}

Node SygusUtil::getSygusNormalized( Node n, std::map< TypeNode, int >& var_count, std::map< Node, Node >& subs ) {
  return n;
  if( n.getKind()==SKOLEM ){
    std::map< Node, Node >::iterator its = subs.find( n );
    if( its!=subs.end() ){
      return its->second;
    }else{
      std::map< Node, TypeNode >::iterator it = d_fv_stype.find( n );
      if( it!=d_fv_stype.end() ){
        Node v = getVarInc( it->second, var_count );
        subs[n] = v;
        return v;
      }else{
        return n;
      }
    }
  }else{
    if( n.getNumChildren()>0 ){
      std::vector< Node > children;
      if( n.getMetaKind() == kind::metakind::PARAMETERIZED ){
        children.push_back( n.getOperator() );
      }
      bool childChanged = false;
      for( unsigned i=0; i<n.getNumChildren(); i++ ){
        Node nc = getSygusNormalized( n[i], var_count, subs );
        childChanged = childChanged || nc!=n[i];
        children.push_back( nc );
      }
      if( childChanged ){
        return NodeManager::currentNM()->mkNode( n.getKind(), children );
      }
    }
    return n;
  }
}

Node SygusUtil::getNormalized( TypeNode t, Node prog, bool do_pre_norm ) {
  if( do_pre_norm ){
    std::map< TypeNode, int > var_count;
    std::map< Node, Node > subs;
    prog = getSygusNormalized( prog, var_count, subs );
  }
  std::map< Node, Node >::iterator itn = d_normalized[t].find( prog );
  if( itn==d_normalized[t].end() ){
    Node progr = Node::fromExpr( smt::currentSmtEngine()->expandDefinitions( prog.toExpr() ) );
    progr = Rewriter::rewrite( progr );
    std::map< TypeNode, int > var_count;
    std::map< Node, Node > subs;
    progr = getSygusNormalized( progr, var_count, subs );
    Trace("sygus-sym-break2") << "...rewrites to " << progr << std::endl;
    d_normalized[t][prog] = progr;
    return progr;
  }else{
    return itn->second;
  }
}

int SygusUtil::getTermSize( Node n ){
  if( isVar( n ) ){
    return 0;
  }else{
    int sum = 0;
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      sum += getTermSize( n[i] );
    }
    return 1+sum;
  }

}

bool SygusUtil::isAssoc( Kind k ) {
  return k==PLUS || k==MULT || k==AND || k==OR || k==XOR || k==IFF ||
         k==BITVECTOR_PLUS || k==BITVECTOR_MULT || k==BITVECTOR_AND || k==BITVECTOR_OR || k==BITVECTOR_XOR || k==BITVECTOR_XNOR || k==BITVECTOR_CONCAT;
}

bool SygusUtil::isComm( Kind k ) {
  return k==PLUS || k==MULT || k==AND || k==OR || k==XOR || k==IFF ||
         k==BITVECTOR_PLUS || k==BITVECTOR_MULT || k==BITVECTOR_AND || k==BITVECTOR_OR || k==BITVECTOR_XOR || k==BITVECTOR_XNOR;
}

bool SygusUtil::isAntisymmetric( Kind k, Kind& dk ) {
  if( k==GT ){
    dk = LT;
    return true;
  }else if( k==GEQ ){
    dk = LEQ;
    return true;
  }else if( k==BITVECTOR_UGT ){
    dk = BITVECTOR_ULT;
    return true;
  }else if( k==BITVECTOR_UGE ){
    dk = BITVECTOR_ULE;
    return true;
  }else if( k==BITVECTOR_SGT ){
    dk = BITVECTOR_SLT;
    return true;
  }else if( k==BITVECTOR_SGE ){
    dk = BITVECTOR_SLE;
    return true;
  }else{
    return false;
  }
}

bool SygusUtil::isIdempotentArg( Node n, Kind ik, int arg ) {
  TypeNode tn = n.getType();
  if( n==getTypeValue( tn, 0 ) ){
    if( ik==PLUS || ik==OR || ik==XOR || ik==BITVECTOR_PLUS || ik==BITVECTOR_OR || ik==BITVECTOR_XOR ){
      return true;
    }else if( ik==MINUS || ik==BITVECTOR_SHL || ik==BITVECTOR_LSHR || ik==BITVECTOR_SUB ){
      return arg==1;
    }
  }else if( n==getTypeValue( tn, 1 ) ){
    if( ik==MULT || ik==BITVECTOR_MULT ){
      return true;
    }else if( ik==DIVISION || ik==BITVECTOR_UDIV || ik==BITVECTOR_SDIV ){
      return arg==1;
    }
  }else if( n==getTypeMaxValue( tn ) ){
    if( ik==IFF || ik==BITVECTOR_AND || ik==BITVECTOR_XNOR ){
      return true;
    }
  }
  return false;
}


bool SygusUtil::isSingularArg( Node n, Kind ik, int arg ) {
  TypeNode tn = n.getType();
  if( n==getTypeValue( tn, 0 ) ){
    if( ik==AND || ik==MULT || ik==BITVECTOR_AND || ik==BITVECTOR_MULT ){
      return true;
    }else if( ik==DIVISION || ik==BITVECTOR_UDIV || ik==BITVECTOR_SDIV ){
      return arg==0;
    }
  }else if( n==getTypeMaxValue( tn ) ){
    if( ik==OR || ik==BITVECTOR_OR ){
      return true;
    }
  }
  return false;
}

bool SygusUtil::hasOffsetArg( Kind ik, int arg, int& offset, Kind& ok ) {
  if( ik==LT ){
    Assert( arg==0 || arg==1 );
    offset = arg==0 ? 1 : -1;
    ok = LEQ;
    return true;
  }else if( ik==BITVECTOR_ULT ){
    Assert( arg==0 || arg==1 );
    offset = arg==0 ? 1 : -1;
    ok = BITVECTOR_ULE;
    return true;
  }else if( ik==BITVECTOR_SLT ){
    Assert( arg==0 || arg==1 );
    offset = arg==0 ? 1 : -1;
    ok = BITVECTOR_SLE;
    return true;
  }
  return false;
}


Node SygusUtil::getTypeValue( TypeNode tn, int val ) {
  std::map< int, Node >::iterator it = d_type_value[tn].find( val );
  if( it==d_type_value[tn].end() ){
    Node n;
    if( tn.isInteger() || tn.isReal() ){
      Rational c(val);
      n = NodeManager::currentNM()->mkConst( c );
    }else if( tn.isBitVector() ){
      unsigned int uv = val;
      BitVector bval(tn.getConst<BitVectorSize>(), uv);
      n = NodeManager::currentNM()->mkConst<BitVector>(bval);
    }else if( tn.isBoolean() ){
      if( val==0 ){
        n = NodeManager::currentNM()->mkConst( false );
      }
    }
    d_type_value[tn][val] = n;
    return n;
  }else{
    return it->second;
  }
}

Node SygusUtil::getTypeMaxValue( TypeNode tn ) {
  std::map< TypeNode, Node >::iterator it = d_type_max_value.find( tn );
  if( it==d_type_max_value.end() ){
    Node n;
    if( tn.isBitVector() ){
      n = bv::utils::mkOnes(tn.getConst<BitVectorSize>());
    }else if( tn.isBoolean() ){
      n = NodeManager::currentNM()->mkConst( true );
    }
    d_type_max_value[tn] = n;
    return n;
  }else{
    return it->second;
  }
}

Node SygusUtil::getTypeValueOffset( TypeNode tn, Node val, int offset, int& status ) {
  std::map< int, Node >::iterator it = d_type_value_offset[tn][val].find( offset );
  if( it==d_type_value_offset[tn][val].end() ){
    Node val_o;
    Node offset_val = getTypeValue( tn, offset );
    status = -1;
    if( !offset_val.isNull() ){
      if( tn.isInteger() || tn.isReal() ){
        val_o = Rewriter::rewrite( NodeManager::currentNM()->mkNode( PLUS, val, offset_val ) );
        status = 0;
      }else if( tn.isBitVector() ){
        val_o = Rewriter::rewrite( NodeManager::currentNM()->mkNode( BITVECTOR_PLUS, val, offset_val ) );
      }
    }
    d_type_value_offset[tn][val][offset] = val_o;
    d_type_value_offset_status[tn][val][offset] = status;
    return val_o;
  }else{
    status = d_type_value_offset_status[tn][val][offset];
    return it->second;
  }
}

void SygusUtil::registerSygusType( TypeNode tn ){
  if( d_register.find( tn )==d_register.end() ){
    if( !DatatypesRewriter::isTypeDatatype( tn ) ){
      d_register[tn] = TypeNode::null();
    }else{
      const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
      Trace("sygus-util") << "Register type " << dt.getName() << "..." << std::endl;
      d_register[tn] = TypeNode::fromType( dt.getSygusType() );
      if( d_register[tn].isNull() ){
        Trace("sygus-util") << "...not sygus." << std::endl;
      }else{
        for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
          Expr sop = dt[i].getSygusOp();
          Assert( !sop.isNull() );
          Node n = Node::fromExpr( sop );
          Trace("sygus-util") << "  Operator #" << i << " : " << sop;
          if( sop.getKind() == kind::BUILTIN ){
            Kind sk = NodeManager::operatorToKind( n );
            Trace("sygus-util") << ", kind = " << sk;
            d_kinds[tn][sk] = i;
            d_arg_kind[tn][i] = sk;
          }else if( sop.isConst() ){
            Trace("sygus-util") << ", constant";
            d_consts[tn][n] = i;
            d_arg_const[tn][i] = n;
          }
          d_ops[tn][n] = i;
          d_arg_ops[tn][i] = n;
          Trace("sygus-util") << std::endl;
        }
      }
    }
  }
}

bool SygusUtil::isRegistered( TypeNode tn ) {
  return d_register.find( tn )!=d_register.end();
}

int SygusUtil::getKindArg( TypeNode tn, Kind k ) {
  Assert( isRegistered( tn ) );
  std::map< TypeNode, std::map< Kind, int > >::iterator itt = d_kinds.find( tn );
  if( itt!=d_kinds.end() ){
    std::map< Kind, int >::iterator it = itt->second.find( k );
    if( it!=itt->second.end() ){
      return it->second;
    }
  }
  return -1;
}

int SygusUtil::getConstArg( TypeNode tn, Node n ){
  Assert( isRegistered( tn ) );
  std::map< TypeNode, std::map< Node, int > >::iterator itt = d_consts.find( tn );
  if( itt!=d_consts.end() ){
    std::map< Node, int >::iterator it = itt->second.find( n );
    if( it!=itt->second.end() ){
      return it->second;
    }
  }
  return -1;
}

int SygusUtil::getOpArg( TypeNode tn, Node n ) {
  std::map< Node, int >::iterator it = d_ops[tn].find( n );
  if( it!=d_ops[tn].end() ){
    return it->second;
  }else{
    return -1;
  }
}

bool SygusUtil::hasKind( TypeNode tn, Kind k ) {
  return getKindArg( tn, k )!=-1;
}
bool SygusUtil::hasConst( TypeNode tn, Node n ) {
  return getConstArg( tn, n )!=-1;
}
bool SygusUtil::hasOp( TypeNode tn, Node n ) {
  return getOpArg( tn, n )!=-1;
}

Node SygusUtil::getArgOp( TypeNode tn, int i ) {
  Assert( isRegistered( tn ) );
  std::map< TypeNode, std::map< int, Node > >::iterator itt = d_arg_ops.find( tn );
  if( itt!=d_arg_ops.end() ){
    std::map< int, Node >::iterator itn = itt->second.find( i );
    if( itn!=itt->second.end() ){
      return itn->second;
    }
  }
  return Node::null();
}

Kind SygusUtil::getArgKind( TypeNode tn, int i ) {
  Assert( isRegistered( tn ) );
  std::map< TypeNode, std::map< int, Kind > >::iterator itt = d_arg_kind.find( tn );
  if( itt!=d_arg_kind.end() ){
    std::map< int, Kind >::iterator itk = itt->second.find( i );
    if( itk!=itt->second.end() ){
      return itk->second;
    }
  }
  return UNDEFINED_KIND;
}

bool SygusUtil::isKindArg( TypeNode tn, int i ) {
  return getArgKind( tn, i )!=UNDEFINED_KIND;
}

bool SygusUtil::isConstArg( TypeNode tn, int i ) {
  Assert( isRegistered( tn ) );
  std::map< TypeNode, std::map< int, Node > >::iterator itt = d_arg_const.find( tn );
  if( itt!=d_arg_const.end() ){
    return itt->second.find( i )!=itt->second.end();
  }else{
    return false;
  }
}




