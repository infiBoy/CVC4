/*********************                                                        */
/*! \file node_manager_listeners.h
 ** \verbatim
 ** Original author: Tim King
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief Listeners that NodeManager registers to its Options object.
 **
 ** Listeners that NodeManager registers to its Options object.
 **/

#include "cvc4_private.h"

#ifndef __CVC4__EXPR__NODE_MANAGER_LISTENERS_H
#define __CVC4__EXPR__NODE_MANAGER_LISTENERS_H

#include "base/listener.h"
#include "util/resource_manager.h"

namespace CVC4 {
namespace expr {

class TlimitListener : public Listener {
 public:
  TlimitListener(ResourceManager* rm) : d_rm(rm) {}
  virtual void notify();
 private:
  ResourceManager* d_rm;
};

class TlimitPerListener : public Listener {
 public:
  TlimitPerListener(ResourceManager* rm) : d_rm(rm) {}
  virtual void notify();
 private:
  ResourceManager* d_rm;
};

class RlimitListener : public Listener {
 public:
  RlimitListener(ResourceManager* rm) : d_rm(rm) {}
  virtual void notify();
 private:
  ResourceManager* d_rm;
};

class RlimitPerListener : public Listener {
 public:
  RlimitPerListener(ResourceManager* rm) : d_rm(rm) {}
  virtual void notify();
 private:
  ResourceManager* d_rm;
};

}/* CVC4::expr namespace */
}/* CVC4 namespace */

#endif /* __CVC4__EXPR__NODE_MANAGER_LISTENERS_H */