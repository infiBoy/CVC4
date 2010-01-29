/*********************                                           -*- C++ -*-  */
/** context.cpp
 ** Original author: mdeters
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 prototype.
 ** Copyright (c) 2009 The Analysis of Computer Systems Group (ACSys)
 ** Courant Institute of Mathematical Sciences
 ** New York University
 ** See the file COPYING in the top-level source directory for licensing
 ** information.
 **
 **/


#include "context/context.h"
#include "util/Assert.h"


namespace CVC4 {
namespace context {


Context::Context() : d_pContextNotifyObj(NULL) {
  // Create new memory manager
  d_pCMM = new ContextMemoryManager();

  // Create initial Scope
  d_pScopeTop = new(cmm) Scope(this, cmm);
  d_pScopeBottom = d_pScopeTop;
}


Context::~Context() {
  // Delete all Scopes
  popto(-1);

  // Delete the memory manager
  delete d_pCMM;

  // Clear ContextNotifyObj lists so there are no dangling pointers
  ContextNotifyObj* pCNO;
  while (d_pCNOpre != NULL) {
    pCNO = d_pCNOpre;
    pCNO->d_ppCNOprev = NULL;
    d_pContextNotifyObj = pCNO->d_pCNOnext;
    pCNO->d_pCNOnext = NULL;
  }
  while (d_pCNOpost != NULL) {
    pCNO = d_pCNOpost;
    pCNO->d_ppCNOprev = NULL;
    d_pContextNotifyObj = pCNO->d_pCNOnext;
    pCNO->d_pCNOnext = NULL;
  }
}


void Context::push() {
  // FIXME: TRACE("pushpop", indentStr, "Push", " {");

  // Create a new memory region
  d_pCMM->push();

  // Create a new top Scope
  d_pScopeTop = new(d_pCMM) Scope(this, d_pCMM, d_pScopeTop);
}


void Context::pop() {
  // Notify the (pre-pop) ContextNotifyObj objects
  ContextNotifyObj* pCNO = d_pCNOPre;
  while (pCNO != NULL) {
    pCNO->notify();
    pCNO = pCNO->d_pCNOnext;
  }

  // Grab the top Scope
  Scope* pScope = d_pScopeTop;

  // Restore the previous Scope
  d_pScopeTop = pScope->getScopePrev();

  // Restore all objects in the top Scope
  delete(d_pCMM) pScope;

  // Pop the memory region
  d_pCMM->pop();

  // Notify the (post-pop) ContextNotifyObj objects
  ContextNotifyObj* pCNO = d_pCNOPost;
  while (pCNO != NULL) {
    pCNO->notify();
    pCNO = pCNO->d_pCNOnext;
  }

  //FIXME:  TRACE("pushpop", indentStr, "}", " Pop");
}


void Context::popto(int toLevel) {
  // Pop scopes until there are none left or toLevel is reached
  while (d_pScopeTop != NULL && toLevel < d_pScopeTop()->getLevel()) pop();
}


void Context::addNotifyObjPre(ContextNotifyObj* pCNO) {
  // Insert pCNO at *front* of list
  if(d_pCNOpre != NULL)
    d_pCNOpre->prev() = &(pCNO->next());
  pCNO->next() = d_pCNOpre;
  pCNO->prev() = &d_pCNOpre;
  d_pCNOpre = pCNO;
}


void Context::addNotifyObjPost(ContextNotifyObj* pCNO) {
  // Insert pCNO at *front* of list
  if(d_pCNOpost != NULL)
    d_pCNOpost->prev() = &(pCNO->next());
  pCNO->next() = d_pCNOpost;
  pCNO->prev() = &d_pCNOpost;
  d_pCNOpost = pCNO;
}


void ContextObj::update() {
  // Call save() to save the information in the current object
  ContextObj* pContextObjSaved = save(d_pScope->getCMM());

  // Check that base class data was saved
  Assert(saved.d_pContextObjNext == d_pContextObjNext &&
         saved.d_ppContextObjPrev == d_ppContextObjPrev &&
         saved.d_pContextObjRestore == d_pContextObjRestore &&
         saved.d_pScope == d_pScope,
         "save() did not properly copy information in base class");

  // Update Scope pointer to current top Scope
  d_pScope = d_pScope->getContext()->getTopScope();

  // Store the saved copy in the restore pointer
  d_pContextObjRestore = pContextObjSaved;

  // Insert object into the list of objects that need to be restored when this
  // Scope is popped.
  d_pScope->addToChain(this);
}


ContextObj* ContextObj::restoreAndContinue()
{
  // Variable to hold next object in list
  ContextObj* pContextObjNext;

  // Check the restore pointer.  If NULL, this must be the bottom Scope
  if (d_pContextObjRestore == NULL) {
    Assert(d_pScope == d_pScope->getContext()->getBottomScope(),
           "Expected bottom scope");
    pContextObjNext = d_pContextObjNext;
    // Nothing else to do
  }
  else {
    // Call restore to update the subclass data
    restore(d_pContextObjRestore);

    // Remember the next object in the list
    pContextObjNext = d_pContextObjNext;

    // Restore the base class data
    d_pScope = d_pContextObjRestore->d_pScope;
    next() = d_pContextObjRestore->d_pContextObjNext;
    prev() = d_pContextObjRestore->d_pContextObjPrev;
    d_pContextObjRestore = d_pContextObjRestore->d_pContextObjRestore;
  }
  // Return the next object in the list
  return pContextObjNext;
}


ContextObj::ContextObj(Context* pContext)
  : d_pContextObjRestore(NULL)
{
  Assert(pContext != NULL, "NULL context pointer");
  d_pScope = pContext->getBottomScope();
  d_pScope->addToChain(this);
}


ContextObj::~ContextObj()
{
  for(;;) {
    if (next() != NULL) {
      next()->prev() = prev();
    }
    *(prev()) = next();
    if (d_pContextObjRestore == NULL) break;
    restoreAndContinue();
  }
}


ContextNotifyObj::ContextNotifyObj(Context* pContext, bool preNotify = false) {
  if (preNotify) {
    pContext->addNotifyObjPre(this);
  }
  else {
    pContext->addNotifyObjPost(this);
  }
}


ContextNotifyObj::~ContextNotifyObj()
{
  if (d_pCNOnext != NULL) {
    d_pCNOnext->d_pCNOprev = d_pCNOprev;
  }
  if (d_pCNOprev != NULL) {
    *(d_pCNOprev) = d_pCNOnext;
  }
}


} /* CVC4::context namespace */


} /* CVC4 namespace */
