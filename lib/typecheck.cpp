/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
 *  Main authors:
 *     Guido Tack <guido.tack@monash.edu>
 */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <minizinc/typecheck.hh>

#include <minizinc/astiterator.hh>
#include <minizinc/astexception.hh>
#include <minizinc/hash.hh>
#include <minizinc/flatten_internal.hh>

#include <string>
#include <sstream>

#include <minizinc/prettyprinter.hh>

namespace MiniZinc {
  
  Scopes::Scopes(void) {
    s.push_back(Scope());
    s.back().toplevel = true;
  }
  
  void
  Scopes::add(EnvI &env, VarDecl *vd) {
    if (!s.back().toplevel && vd->ti()->isEnum() && vd->e()) {
      throw TypeError(env, vd->loc(), "enums are only allowed at top level");
    }
    if (vd->id()->idn()==-1 && vd->id()->v()=="")
      return;
    DeclMap::iterator vdi = s.back().m.find(vd->id());
    if (vdi == s.back().m.end()) {
      s.back().m.insert(vd->id(),vd);
    } else {
      GCLock lock;
      throw TypeError(env, vd->loc(),"identifier `"+vd->id()->str().str()+
                      "' already defined");
    }
  }
  
  void
  Scopes::push(bool toplevel) {
    s.push_back(Scope());
    s.back().toplevel = toplevel;
  }
  
  void
  Scopes::pop(void) {
    s.pop_back();
  }
  
  VarDecl*
  Scopes::find(Id *ident) {
    int cur = s.size()-1;
    for (;;) {
      DeclMap::iterator vdi = s[cur].m.find(ident);
      if (vdi == s[cur].m.end()) {
        if (s[cur].toplevel) {
          if (cur > 0)
            cur = 0;
          else
            return NULL;
        } else {
          cur--;
        }
      } else {
        return vdi->second;
      }
    }
  }
  
  struct VarDeclCmp {
    UNORDERED_NAMESPACE::unordered_map<VarDecl*,int>& _pos;
    VarDeclCmp(UNORDERED_NAMESPACE::unordered_map<VarDecl*,int>& pos) : _pos(pos) {}
    bool operator()(Expression* e0, Expression* e1) {
      if (VarDecl* vd0 = Expression::dyn_cast<VarDecl>(e0)) {
        if (VarDecl* vd1 = Expression::dyn_cast<VarDecl>(e1)) {
          return _pos[vd0] < _pos[vd1];
        } else {
          return true;
        }
      } else {
        return false;
      }
    }
  };
  struct ItemCmp {
    UNORDERED_NAMESPACE::unordered_map<VarDecl*,int>& _pos;
    ItemCmp(UNORDERED_NAMESPACE::unordered_map<VarDecl*,int>& pos) : _pos(pos) {}
    bool operator()(Item* i0, Item* i1) {
      if (VarDeclI* vd0 = i0->cast<VarDeclI>()) {
        if (VarDeclI* vd1 = i1->cast<VarDeclI>()) {
          return _pos[vd0->e()] < _pos[vd1->e()];
        } else {
          return true;
        }
      } else {
        return false;
      }
    }
  };
  
  std::string createEnumToStringName(Id* ident, std::string prefix) {
    std::string name = ident->str().str();
    if (name[0]=='\'') {
      name = "'"+prefix+name.substr(1);
    } else {
      name = prefix+name;
    }
    return name;
  }
  
  AssignI* createEnumMapper(EnvI& env, Model* m, unsigned int enumId, VarDecl* vd, VarDecl* vd_enumToString, Model* enumItems) {

    Id* ident = vd->id();
    SetLit* sl = NULL;
    
    AssignI* ret = NULL;
    
    GCLock lock;
    if (Call* c = vd->e()->dyn_cast<Call>()) {
      if (c->id()!="anon_enum") {
        throw TypeError(env, c->loc(),
                        "invalid initialisation for enum `"+ident->v().str()+"'");
      }
    } else if ( (sl = vd->e()->dyn_cast<SetLit>()) ) {
      for (unsigned int i=0; i<sl->v().size(); i++) {
        if (!sl->v()[i]->isa<Id>()) {
          throw TypeError(env, sl->v()[i]->loc(),
                          "invalid initialisation for enum `"+ident->v().str()+"'");
        }
        TypeInst* ti_id = new TypeInst(sl->v()[i]->loc(),Type::parenum(enumId));
        
        std::vector<Expression*> toEnumArgs(2);
        toEnumArgs[0] = vd->id();
        toEnumArgs[1] = IntLit::a(i+1);
        Call* toEnum = new Call(sl->v()[i]->loc(), ASTString("to_enum"), toEnumArgs);
        toEnum->decl(env.orig->matchFn(env, toEnum, false));
        VarDecl* vd_id = new VarDecl(ti_id->loc(),ti_id,sl->v()[i]->cast<Id>()->str(),toEnum);
        enumItems->addItem(new VarDeclI(vd_id->loc(),vd_id));
      }
      SetLit* nsl = new SetLit(vd->loc(), IntSetVal::a(1,sl->v().size()));
      Type tt = nsl->type();
      tt.enumId(vd->type().enumId());
      nsl->type(tt);
      vd->e(nsl);
    } else {
      throw TypeError(env, vd->e()->loc(),
                      "invalid initialisation for enum `"+ident->v().str()+"'");
    }

    
    if (sl) {
      std::string name = createEnumToStringName(ident,"_enum_to_string_");
      std::vector<Expression*> al_args(sl->v().size());
      for (unsigned int i=0; i<sl->v().size(); i++) {
        al_args[i] = new StringLit(Location().introduce(),sl->v()[i]->cast<Id>()->str());
      }
      ArrayLit* al = new ArrayLit(Location().introduce(),al_args);
      
      if (vd_enumToString) {
        ret = new AssignI(Location().introduce(),name,al);
        ret->decl(vd_enumToString);
      } else {
        std::vector<TypeInst*> ranges(1);
        ranges[0] = new TypeInst(Location().introduce(),Type::parint());
        TypeInst* ti = new TypeInst(Location().introduce(),Type::parstring(1));
        ti->setRanges(ranges);
        vd_enumToString = new VarDecl(Location().introduce(),ti,name,al);
        enumItems->addItem(new VarDeclI(Location().introduce(),vd_enumToString));
      }
      
      TypeInst* ti_aa = new TypeInst(Location().introduce(),Type::parint());
      VarDecl* vd_aa = new VarDecl(Location().introduce(),ti_aa,"x");
      vd_aa->toplevel(false);
      TypeInst* ti_ab = new TypeInst(Location().introduce(),Type::parbool());
      VarDecl* vd_ab = new VarDecl(Location().introduce(),ti_ab,"b");
      vd_ab->toplevel(false);
      std::vector<Expression*> aa_args(1);
      aa_args[0] = vd_aa->id();
      ArrayAccess* aa = new ArrayAccess(Location().introduce(),vd_enumToString->id(),aa_args);
      TypeInst* ti_fi = new TypeInst(Location().introduce(),Type::parstring());
      std::vector<VarDecl*> fi_params(2);
      fi_params[0] = vd_aa;
      fi_params[1] = vd_ab;
      FunctionI* fi = new FunctionI(Location().introduce(),
                                    createEnumToStringName(ident, "_toString_"),
                                    ti_fi,fi_params,aa);
      enumItems->addItem(fi);
    } else {
      if (vd_enumToString) {
        /// TODO: find a better solution (don't introduce the vd_enumToString until we
        ///       know it's a non-anonymous enum)
        vd_enumToString->e(new ArrayLit(Location().introduce(), std::vector<Expression*>()));
      }
      {
        TypeInst* ti_aa = new TypeInst(Location().introduce(),Type::parint());
        VarDecl* vd_aa = new VarDecl(Location().introduce(),ti_aa,"x");
        vd_aa->toplevel(false);
        
        TypeInst* ti_ab = new TypeInst(Location().introduce(),Type::parbool());
        VarDecl* vd_ab = new VarDecl(Location().introduce(),ti_ab,"b");
        vd_ab->toplevel(false);

        StringLit* sl_dzn = new StringLit(Location().introduce(),
                                          ASTString("to_enum("+ident->str().str()+","));
        std::vector<Expression*> showIntArgs(1);
        showIntArgs[0] = vd_aa->id();
        Call* showInt_dzn = new Call(Location().introduce(), constants().ids.show, showIntArgs);
        BinOp* construct_string_dzn = new BinOp(Location().introduce(), sl_dzn, BOT_PLUSPLUS, showInt_dzn);
        StringLit* closing_bracket = new StringLit(Location().introduce(), ASTString(")"));
        BinOp* construct_string_dzn_2 = new BinOp(Location().introduce(), construct_string_dzn,
                                             BOT_PLUSPLUS, closing_bracket);
        
        StringLit* sl = new StringLit(Location().introduce(), ASTString(ident->str().str()+"_"));
        Call* showInt = new Call(Location().introduce(), constants().ids.show, showIntArgs);
        BinOp* construct_string = new BinOp(Location().introduce(), sl, BOT_PLUSPLUS, showInt);
        
        std::vector<Expression*> if_then(2);
        if_then[0] = vd_ab->id();
        if_then[1] = construct_string_dzn_2;
        ITE* ite = new ITE(Location().introduce(), if_then, construct_string);
        
        
        TypeInst* ti_fi = new TypeInst(Location().introduce(),Type::parstring());
        std::vector<VarDecl*> fi_params(2);
        fi_params[0] = vd_aa;
        fi_params[1] = vd_ab;
        FunctionI* fi = new FunctionI(Location().introduce(),
                                      createEnumToStringName(ident, "_toString_"),
                                      ti_fi,fi_params,ite);
        enumItems->addItem(fi);
      }
    }
    
    {
      /*
       
       function _toString_ENUM(array[$U] of ENUM: x, bool: b) =
         let {
           array[int] of ENUM: xx = array1d(x)
         } in "[" ++ join(", ", [ _toString_ENUM(xx[i],b) | i in index_set(xx) ]) ++ "]";
       
       */

      TIId* tiid = new TIId(Location().introduce(),"U");
      TypeInst* ti_range = new TypeInst(Location().introduce(),Type::parint(),tiid);
      std::vector<TypeInst*> ranges(1);
      ranges[0] = ti_range;

      TypeInst* x_ti = new TypeInst(Location().introduce(),Type::parint(-1),ranges,ident);
      VarDecl* vd_x = new VarDecl(Location().introduce(),x_ti,"x");
      vd_x->toplevel(false);

      TypeInst* b_ti = new TypeInst(Location().introduce(),Type::parbool());
      VarDecl* vd_b = new VarDecl(Location().introduce(),b_ti,"b");
      vd_b->toplevel(false);

      TypeInst* xx_range = new TypeInst(Location().introduce(),Type::parint(),NULL);
      std::vector<TypeInst*> xx_ranges(1);
      xx_ranges[0] = xx_range;
      TypeInst* xx_ti = new TypeInst(Location().introduce(),Type::parint(1),xx_ranges);
      
      std::vector<Expression*> array1dArgs(1);
      array1dArgs[0] = vd_x->id();
      Call* array1dCall = new Call(Location().introduce(),"array1d",array1dArgs);
      
      VarDecl* vd_xx = new VarDecl(Location().introduce(),xx_ti,"xx",array1dCall);
      vd_xx->toplevel(false);

      TypeInst* idx_i_ti = new TypeInst(Location().introduce(),Type::parint());
      VarDecl* idx_i = new VarDecl(Location().introduce(),idx_i_ti,"i");
      idx_i->toplevel(false);
      
      std::vector<Expression*> aa_xxi_idx(1);
      aa_xxi_idx[0] = idx_i->id();
      ArrayAccess* aa_xxi = new ArrayAccess(Location().introduce(),vd_xx->id(),aa_xxi_idx);
      
      std::vector<Expression*> _toString_ENUMArgs(2);
      _toString_ENUMArgs[0] = aa_xxi;
      _toString_ENUMArgs[1] = vd_b->id();
      Call* _toString_ENUM = new Call(Location().introduce(),
                                      createEnumToStringName(ident, "_toString_"),
                                      _toString_ENUMArgs);
      
      std::vector<Expression*> index_set_xx_args(1);
      index_set_xx_args[0] = vd_xx->id();
      Call* index_set_xx = new Call(Location().introduce(),"index_set",index_set_xx_args);
      std::vector<VarDecl*> gen_exps(1);
      gen_exps[0] = idx_i;
      Generator gen(gen_exps,index_set_xx);
      
      Generators generators;
      generators._g.push_back(gen);
      Comprehension* comp = new Comprehension(Location().introduce(),_toString_ENUM,generators,false);
      
      std::vector<Expression*> join_args(2);
      join_args[0] = new StringLit(Location().introduce(),", ");
      join_args[1] = comp;
      Call* join = new Call(Location().introduce(),"join",join_args);
      
      StringLit* sl_open = new StringLit(Location().introduce(),"[");
      BinOp* bopp0 = new BinOp(Location().introduce(),sl_open,BOT_PLUSPLUS,join);
      StringLit* sl_close = new StringLit(Location().introduce(),"]");
      BinOp* bopp1 = new BinOp(Location().introduce(),bopp0,BOT_PLUSPLUS,sl_close);

      std::vector<Expression*> let_args(1);
      let_args[0] = vd_xx;
      Let* let = new Let(Location().introduce(),let_args,bopp1);
      
      TypeInst* ti_fi = new TypeInst(Location().introduce(),Type::parstring());
      std::vector<VarDecl*> fi_params(2);
      fi_params[0] = vd_x;
      fi_params[1] = vd_b;
      FunctionI* fi = new FunctionI(Location().introduce(),
                                    createEnumToStringName(ident, "_toString_"),
                                    ti_fi,fi_params,let);
      enumItems->addItem(fi);
    }
    
    {
      /*
       
       function _toString_ENUM(set of ENUM: x, bool: b) =
         "{" ++ join(", ", [ _toString_ENUM(i,b) | i in x ]) ++ "}";
       
       */
      
      Type argType = Type::parsetenum(ident->type().enumId());
      TypeInst* x_ti = new TypeInst(Location().introduce(),argType,ident);
      VarDecl* vd_x = new VarDecl(Location().introduce(),x_ti,"x");
      vd_x->toplevel(false);
      
      TypeInst* b_ti = new TypeInst(Location().introduce(),Type::parbool());
      VarDecl* vd_b = new VarDecl(Location().introduce(),b_ti,"b");
      vd_b->toplevel(false);

      TypeInst* idx_i_ti = new TypeInst(Location().introduce(),Type::parint());
      VarDecl* idx_i = new VarDecl(Location().introduce(),idx_i_ti,"i");
      idx_i->toplevel(false);
      
      std::vector<Expression*> _toString_ENUMArgs(2);
      _toString_ENUMArgs[0] = idx_i->id();
      _toString_ENUMArgs[1] = vd_b->id();
      Call* _toString_ENUM = new Call(Location().introduce(),
                                      createEnumToStringName(ident, "_toString_"),
                                      _toString_ENUMArgs);
      
      std::vector<VarDecl*> gen_exps(1);
      gen_exps[0] = idx_i;
      Generator gen(gen_exps,vd_x->id());
      
      Generators generators;
      generators._g.push_back(gen);
      Comprehension* comp = new Comprehension(Location().introduce(),_toString_ENUM,generators,false);
      
      std::vector<Expression*> join_args(2);
      join_args[0] = new StringLit(Location().introduce(),", ");
      join_args[1] = comp;
      Call* join = new Call(Location().introduce(),"join",join_args);
      
      StringLit* sl_open = new StringLit(Location().introduce(),"{");
      BinOp* bopp0 = new BinOp(Location().introduce(),sl_open,BOT_PLUSPLUS,join);
      StringLit* sl_close = new StringLit(Location().introduce(),"}");
      BinOp* bopp1 = new BinOp(Location().introduce(),bopp0,BOT_PLUSPLUS,sl_close);
      
      TypeInst* ti_fi = new TypeInst(Location().introduce(),Type::parstring());
      std::vector<VarDecl*> fi_params(2);
      fi_params[0] = vd_x;
      fi_params[1] = vd_b;
      FunctionI* fi = new FunctionI(Location().introduce(),
                                    createEnumToStringName(ident, "_toString_"),
                                    ti_fi,fi_params,bopp1);
      enumItems->addItem(fi);
    }
    
    return ret;
  }
  
  void
  TopoSorter::add(EnvI& env, VarDeclI* vdi, bool handleEnums, Model* enumItems) {
    VarDecl* vd = vdi->e();
    if (handleEnums && vd->ti()->isEnum()) {
      unsigned int enumId = env.registerEnum(vdi);
      Type vdt = vd->type();
      vdt.enumId(enumId);
      vd->ti()->type(vdt);
      vd->type(vdt);

      if (vd->e()) {
        (void) createEnumMapper(env, model, enumId, vd, NULL, enumItems);
      } else {
        GCLock lock;
        std::string name = createEnumToStringName(vd->id(),"_enum_to_string_");
        std::vector<TypeInst*> ranges(1);
        ranges[0] = new TypeInst(Location().introduce(),Type::parint());
        TypeInst* ti = new TypeInst(Location().introduce(),Type::parstring(1));
        ti->setRanges(ranges);
        VarDecl* vd_enumToString = new VarDecl(Location().introduce(),ti,name,NULL);
        enumItems->addItem(new VarDeclI(Location().introduce(),vd_enumToString));
      }
    }
    scopes.add(env, vd);
  }

  VarDecl*
  TopoSorter::get(EnvI& env, const ASTString& id_v, const Location& loc) {
    GCLock lock;
    Id* id = new Id(Location(), id_v, NULL);
    VarDecl* decl = scopes.find(id);
    if (decl==NULL) {
      throw TypeError(env,loc,"undefined identifier `"+id->str().str()+"'");
    }
    return decl;
  }

  VarDecl*
  TopoSorter::checkId(EnvI& env, Id* ident, const Location& loc) {
    VarDecl* decl = scopes.find(ident);
    if (decl==NULL) {
      GCLock lock;
      throw TypeError(env,loc,"undefined identifier `"+ident->str().str()+"'");
    }
    PosMap::iterator pi = pos.find(decl);
    if (pi==pos.end()) {
      // new id
      scopes.push(true);
      run(env, decl);
      scopes.pop();
    } else {
      // previously seen, check if circular
      if (pi->second==-1) {
        GCLock lock;
        throw TypeError(env,loc,"circular definition of `"+ident->str().str()+"'");
      }
    }
    return decl;
  }

  VarDecl*
  TopoSorter::checkId(EnvI& env, const ASTString& id_v, const Location& loc) {
    GCLock lock;
    Id* id = new Id(loc,id_v,NULL);
    return checkId(env, id, loc);
  }

  void
  TopoSorter::run(EnvI& env, Expression* e) {
    if (e==NULL)
      return;
    switch (e->eid()) {
    case Expression::E_INTLIT:
    case Expression::E_FLOATLIT:
    case Expression::E_BOOLLIT:
    case Expression::E_STRINGLIT:
    case Expression::E_ANON:
      break;
    case Expression::E_SETLIT:
      {
        SetLit* sl = e->cast<SetLit>();
        if(sl->isv()==NULL && sl->fsv()==NULL)
          for (unsigned int i=0; i<sl->v().size(); i++)
            run(env,sl->v()[i]);
      }
      break;
    case Expression::E_ID:
      {
        if (e != constants().absent) {
          VarDecl* vd = checkId(env, e->cast<Id>(),e->loc());
          e->cast<Id>()->decl(vd);
        }
      }
      break;
    case Expression::E_ARRAYLIT:
      {
        ArrayLit* al = e->cast<ArrayLit>();
        for (unsigned int i=0; i<al->v().size(); i++)
          run(env, al->v()[i]);
      }
      break;
    case Expression::E_ARRAYACCESS:
      {
        ArrayAccess* ae = e->cast<ArrayAccess>();
        run(env, ae->v());
        for (unsigned int i=0; i<ae->idx().size(); i++)
          run(env, ae->idx()[i]);
      }
      break;
    case Expression::E_COMP:
      {
        Comprehension* ce = e->cast<Comprehension>();
        scopes.push(false);
        for (int i=0; i<ce->n_generators(); i++) {
          run(env, ce->in(i));
          for (int j=0; j<ce->n_decls(i); j++) {
            run(env, ce->decl(i,j));
            scopes.add(env, ce->decl(i,j));
          }
        }
        if (ce->where())
          run(env, ce->where());
        run(env, ce->e());
        scopes.pop();
      }
      break;
    case Expression::E_ITE:
      {
        ITE* ite = e->cast<ITE>();
        for (int i=0; i<ite->size(); i++) {
          run(env, ite->e_if(i));
          run(env, ite->e_then(i));
        }
        run(env, ite->e_else());
      }
      break;
    case Expression::E_BINOP:
      {
        BinOp* be = e->cast<BinOp>();
        std::vector<Expression*> todo;
        todo.push_back(be->lhs());
        todo.push_back(be->rhs());
        while (!todo.empty()) {
          Expression* e = todo.back();
          todo.pop_back();
          if (BinOp* e_bo = e->dyn_cast<BinOp>()) {
            todo.push_back(e_bo->lhs());
            todo.push_back(e_bo->rhs());
            for (ExpressionSetIter it = e_bo->ann().begin(); it != e_bo->ann().end(); ++it)
              run(env, *it);
          } else {
            run(env, e);
          }
        }
      }
      break;
    case Expression::E_UNOP:
      {
        UnOp* ue = e->cast<UnOp>();
        run(env, ue->e());
      }
      break;
    case Expression::E_CALL:
      {
        Call* ce = e->cast<Call>();
        for (unsigned int i=0; i<ce->args().size(); i++)
          run(env, ce->args()[i]);
      }
      break;
    case Expression::E_VARDECL:
      {
        VarDecl* ve = e->cast<VarDecl>();
        PosMap::iterator pi = pos.find(ve);
        if (pi==pos.end()) {
          pos.insert(std::pair<VarDecl*,int>(ve,-1));
          run(env, ve->ti());
          run(env, ve->e());
          ve->payload(decls.size());
          decls.push_back(ve);
          pi = pos.find(ve);
          pi->second = decls.size()-1;
        } else {
          assert(pi->second != -1);
        }
      }
      break;
    case Expression::E_TI:
      {
        TypeInst* ti = e->cast<TypeInst>();
        for (unsigned int i=0; i<ti->ranges().size(); i++)
          run(env, ti->ranges()[i]);
        run(env, ti->domain());
      }
      break;
    case Expression::E_TIID:
      break;
    case Expression::E_LET:
      {
        Let* let = e->cast<Let>();
        scopes.push(false);
        for (unsigned int i=0; i<let->let().size(); i++) {
          run(env, let->let()[i]);
          if (VarDecl* vd = let->let()[i]->dyn_cast<VarDecl>()) {
            scopes.add(env, vd);
          }
        }
        run(env, let->in());
        VarDeclCmp poscmp(pos);
        std::stable_sort(let->let().begin(), let->let().end(), poscmp);
        for (unsigned int i=0; i<let->let().size(); i++) {
          if (VarDecl* vd = let->let()[i]->dyn_cast<VarDecl>()) {
            let->let_orig()[i] = vd->e();
          } else {
            let->let_orig()[i] = NULL;
          }
        }
        scopes.pop();
      }
      break;
    }
    for (ExpressionSetIter it = e->ann().begin(); it != e->ann().end(); ++it)
      run(env, *it);
  }
  
  KeepAlive addCoercion(EnvI& env, Model* m, Expression* e, const Type& funarg_t) {
    if (e->type().dim()==funarg_t.dim() && (funarg_t.bt()==Type::BT_BOT || funarg_t.bt()==Type::BT_TOP || e->type().bt()==funarg_t.bt() || e->type().bt()==Type::BT_BOT))
      return e;
    std::vector<Expression*> args(1);
    args[0] = e;
    GCLock lock;
    Call* c = NULL;
    if (e->type().dim()==0 && funarg_t.dim()!=0) {
      if (e->type().isvar()) {
        throw TypeError(env, e->loc(),"cannot coerce var set into array");
      }
      std::vector<Expression*> set2a_args(1);
      set2a_args[0] = e;
      Call* set2a = new Call(e->loc(), ASTString("set2array"), set2a_args);
      FunctionI* fi = m->matchFn(env, set2a, false);
      assert(fi);
      set2a->type(fi->rtype(env, args, false));
      set2a->decl(fi);
      e = set2a;
    }
    if (funarg_t.bt()==Type::BT_TOP || e->type().bt()==funarg_t.bt() || e->type().bt()==Type::BT_BOT) {
      KeepAlive ka(e);
      return ka;
    }
    if (e->type().bt()==Type::BT_BOOL) {
      if (funarg_t.bt()==Type::BT_INT) {
        c = new Call(e->loc(), constants().ids.bool2int, args);
      } else if (funarg_t.bt()==Type::BT_FLOAT) {
        c = new Call(e->loc(), constants().ids.bool2float, args);
      }
    } else if (e->type().bt()==Type::BT_INT) {
      if (funarg_t.bt()==Type::BT_FLOAT) {
        c = new Call(e->loc(), constants().ids.int2float, args);
      }
    }
    if (c) {
      FunctionI* fi = m->matchFn(env, c, false);
      assert(fi);
      c->type(fi->rtype(env, args, false));
      c->decl(fi);
      KeepAlive ka(c);
      return ka;
    }
    throw TypeError(env, e->loc(),"cannot determine coercion from type "+e->type().toString(env)+" to type "+funarg_t.toString(env));
  }
  KeepAlive addCoercion(EnvI& env, Model* m, Expression* e, Expression* funarg) {
    return addCoercion(env, m, e, funarg->type());
  }
  
  template<bool ignoreVarDecl>
  class Typer {
  public:
    EnvI& _env;
    Model* _model;
    std::vector<TypeError>& _typeErrors;
    Typer(EnvI& env, Model* model, std::vector<TypeError>& typeErrors) : _env(env), _model(model), _typeErrors(typeErrors) {}
    /// Check annotations when expression is finished
    void exit(Expression* e) {
      for (ExpressionSetIter it = e->ann().begin(); it != e->ann().end(); ++it)
        if (!(*it)->type().isann())
          throw TypeError(_env,(*it)->loc(),"expected annotation, got `"+(*it)->type().toString(_env)+"'");
    }
    bool enter(Expression*) { return true; }
    /// Visit integer literal
    void vIntLit(const IntLit&) {}
    /// Visit floating point literal
    void vFloatLit(const FloatLit&) {}
    /// Visit Boolean literal
    void vBoolLit(const BoolLit&) {}
    /// Visit set literal
    void vSetLit(SetLit& sl) {
      Type ty; ty.st(Type::ST_SET);
      if (sl.isv()) {
        ty.bt(Type::BT_INT);
        ty.enumId(sl.type().enumId());
        sl.type(ty);
        return;
      }
      unsigned int enumId = sl.v().size() > 0 ? sl.v()[0]->type().enumId() : 0;
      for (unsigned int i=0; i<sl.v().size(); i++) {
        if (sl.v()[i]->type().dim() > 0)
          throw TypeError(_env,sl.v()[i]->loc(),"set literals cannot contain arrays");
        if (sl.v()[i]->type().isvar())
          ty.ti(Type::TI_VAR);
        if (sl.v()[i]->type().cv())
          ty.cv(true);
        if (enumId != sl.v()[i]->type().enumId())
          enumId = 0;
        if (!Type::bt_subtype(sl.v()[i]->type(), ty, true)) {
          if (ty.bt() == Type::BT_UNKNOWN || Type::bt_subtype(ty, sl.v()[i]->type(), true)) {
            ty.bt(sl.v()[i]->type().bt());
          } else {
            throw TypeError(_env,sl.loc(),"non-uniform set literal");
          }
        }
      }
      ty.enumId(enumId);
      if (ty.bt() == Type::BT_UNKNOWN) {
        ty.bt(Type::BT_BOT);
      } else {
        if (ty.isvar() && ty.bt()!=Type::BT_INT) {
          if (ty.bt()==Type::BT_BOOL)
            ty.bt(Type::BT_INT);
          else
            throw TypeError(_env,sl.loc(),"cannot coerce set literal element to var int");
        }
        for (unsigned int i=0; i<sl.v().size(); i++) {
          sl.v()[i] = addCoercion(_env, _model, sl.v()[i], ty)();
        }
      }
      sl.type(ty);
    }
    /// Visit string literal
    void vStringLit(const StringLit&) {}
    /// Visit identifier
    void vId(Id& id) {
      if (&id != constants().absent) {
        assert(!id.decl()->type().isunknown());
        id.type(id.decl()->type());
      }
    }
    /// Visit anonymous variable
    void vAnonVar(const AnonVar&) {}
    /// Visit array literal
    void vArrayLit(ArrayLit& al) {
      Type ty; ty.dim(al.dims());
      std::vector<AnonVar*> anons;
      bool haveInferredType = false;
      for (unsigned int i=0; i<al.v().size(); i++) {
        Expression* vi = al.v()[i];
        if (vi->type().dim() > 0)
          throw TypeError(_env,vi->loc(),"arrays cannot be elements of arrays");
        
        AnonVar* av = vi->dyn_cast<AnonVar>();
        if (av) {
          ty.ti(Type::TI_VAR);
          anons.push_back(av);
        } else if (vi->type().isvar()) {
          ty.ti(Type::TI_VAR);
        }
        if (vi->type().cv())
          ty.cv(true);
        if (vi->type().isopt()) {
          ty.ot(Type::OT_OPTIONAL);
        }
        
        if (ty.bt()==Type::BT_UNKNOWN) {
          if (av == NULL) {
            if (haveInferredType) {
              if (ty.st() != vi->type().st()) {
                throw TypeError(_env,al.loc(),"non-uniform array literal");
              }
              if (ty.enumId() != vi->type().enumId()) {
                ty.enumId(0);
              }
            } else {
              haveInferredType = true;
              ty.st(vi->type().st());
              ty.enumId(vi->type().enumId());
            }
            if (vi->type().bt() != Type::BT_BOT) {
              ty.bt(vi->type().bt());
            }
          }
        } else {
          if (av == NULL) {
            if (vi->type().bt() == Type::BT_BOT) {
              if (vi->type().st() != ty.st()) {
                throw TypeError(_env,al.loc(),"non-uniform array literal");
              }
              if (vi->type().enumId() != 0 && ty.enumId() != vi->type().enumId()) {
                ty.enumId(0);
              }
            } else {
              unsigned int tyEnumId = ty.enumId();
              ty.enumId(vi->type().enumId());
              if (Type::bt_subtype(ty, vi->type(), true)) {
                ty.bt(vi->type().bt());
              }
              if (tyEnumId != vi->type().enumId())
                ty.enumId(0);
              if (!Type::bt_subtype(vi->type(),ty,true) || ty.st() != vi->type().st()) {
                throw TypeError(_env,al.loc(),"non-uniform array literal");
              }
            }
          }
        }
      }
      if (ty.bt() == Type::BT_UNKNOWN) {
        ty.bt(Type::BT_BOT);
        if (!anons.empty())
          throw TypeError(_env,al.loc(),"array literal must contain at least one non-anonymous variable");
      } else {
        Type at = ty;
        at.dim(0);
        if (at.ti()==Type::TI_VAR && at.st()==Type::ST_SET && at.bt()!=Type::BT_INT) {
          if (at.bt()==Type::BT_BOOL) {
            ty.bt(Type::BT_INT);
            at.bt(Type::BT_INT);
          } else {
            throw TypeError(_env,al.loc(),"cannot coerce array element to var set of int");
          }
        }
        for (unsigned int i=0; i<anons.size(); i++) {
          anons[i]->type(at);
        }
        for (unsigned int i=0; i<al.v().size(); i++) {
          al.v()[i] = addCoercion(_env, _model, al.v()[i], at)();
        }
      }
      if (ty.enumId() != 0) {
        std::vector<unsigned int> enumIds(ty.dim()+1);
        for (unsigned int i=0; i<ty.dim(); i++)
          enumIds[i] = 0;
        enumIds[ty.dim()] = ty.enumId();
        ty.enumId(_env.registerArrayEnum(enumIds));
      }
      al.type(ty);
    }
    /// Visit array access
    void vArrayAccess(ArrayAccess& aa) {
      if (aa.v()->type().dim()==0) {
        if (aa.v()->type().st() == Type::ST_SET) {
          Type tv = aa.v()->type();
          tv.st(Type::ST_PLAIN);
          tv.dim(1);
          aa.v(addCoercion(_env, _model, aa.v(), tv)());
        } else {
          std::ostringstream oss;
          oss << "array access attempted on expression of type `" << aa.v()->type().toString(_env) << "'";
          throw TypeError(_env,aa.v()->loc(),oss.str());
        }
      }
      if (aa.v()->type().dim() != aa.idx().size()) {
        std::ostringstream oss;
        oss << aa.v()->type().dim() << "-dimensional array accessed with "
        << aa.idx().size() << (aa.idx().size()==1 ? " expression" : " expressions");
        throw TypeError(_env,aa.v()->loc(),oss.str());
      }
      Type tt = aa.v()->type();
      if (tt.enumId() != 0) {
        const std::vector<unsigned int>& arrayEnumIds = _env.getArrayEnum(tt.enumId());
        
        for (unsigned int i=0; i<arrayEnumIds.size()-1; i++) {
          if (arrayEnumIds[i] != 0) {
            if (aa.idx()[i]->type().enumId() != arrayEnumIds[i]) {
              std::ostringstream oss;
              oss << "array index ";
              if (aa.idx().size() > 1) {
                oss << (i+1) << " ";
              }
              oss << "must be `" << _env.getEnum(arrayEnumIds[i])->e()->id()->str().str() << "', but is `" << aa.idx()[i]->type().toString(_env) << "'";
              throw TypeError(_env,aa.loc(),oss.str());
            }
          }
        }
        tt.enumId(arrayEnumIds[arrayEnumIds.size()-1]);
      }
      tt.dim(0);
      for (unsigned int i=0; i<aa.idx().size(); i++) {
        Expression* aai = aa.idx()[i];
        if (aai->isa<AnonVar>()) {
          aai->type(Type::varint());
        }
        if (aai->type().is_set() || (aai->type().bt() != Type::BT_INT && aai->type().bt() != Type::BT_BOOL) || aai->type().dim() != 0) {
          throw TypeError(_env,aa.loc(),"array index must be `int', but is `"+aai->type().toString(_env)+"'");
        }
        aa.idx()[i] = addCoercion(_env, _model, aai, Type::varint())();
        if (aai->type().isopt()) {
          tt.ot(Type::OT_OPTIONAL);
        }
        if (aai->type().isvar()) {
          tt.ti(Type::TI_VAR);
          if (tt.bt()==Type::BT_ANN || tt.bt()==Type::BT_STRING) {
            throw TypeError(_env,aai->loc(),std::string("array access using a variable not supported for array of ")+(tt.bt()==Type::BT_ANN ? "ann" : "string"));
          }
        }
        if (aai->type().cv())
          tt.cv(true);
      }
      aa.type(tt);
    }
    /// Visit array comprehension
    void vComprehension(Comprehension& c) {
      Type tt = c.e()->type();
      for (int i=0; i<c.n_generators(); i++) {
        Expression* g_in = c.in(i);
        const Type& ty_in = g_in->type();
        if (ty_in == Type::varsetint()) {
          tt.ot(Type::OT_OPTIONAL);
          tt.ti(Type::TI_VAR);
        }
        if (ty_in.cv())
          tt.cv(true);
      }
      if (c.where()) {
        if (c.where()->type() == Type::varbool()) {
          tt.ot(Type::OT_OPTIONAL);
          tt.ti(Type::TI_VAR);
        } else if (c.where()->type() != Type::parbool()) {
          throw TypeError(_env,c.where()->loc(),
                          "where clause must be bool, but is `"+
                          c.where()->type().toString(_env)+"'");
        }
        if (c.where()->type().cv())
          tt.cv(true);
      }
      if (c.set()) {
        if (c.e()->type().dim() != 0 || c.e()->type().st() == Type::ST_SET)
          throw TypeError(_env,c.e()->loc(),
              "set comprehension expression must be scalar, but is `"
              +c.e()->type().toString(_env)+"'");
        tt.st(Type::ST_SET);
        if (tt.isvar()) {
          c.e(addCoercion(_env, _model, c.e(), Type::varint())());
          tt.bt(Type::BT_INT);
        }
      } else {
        if (c.e()->type().dim() != 0)
          throw TypeError(_env,c.e()->loc(),
            "array comprehension expression cannot be an array");
        tt.dim(1);
        if (tt.enumId() != 0) {
          std::vector<unsigned int> enumIds(2);
          enumIds[0] = 0;
          enumIds[1] = tt.enumId();
          tt.enumId(_env.registerArrayEnum(enumIds));
        }
      }
      c.type(tt);
    }
    /// Visit array comprehension generator
    void vComprehensionGenerator(Comprehension& c, int gen_i) {
      Expression* g_in = c.in(gen_i);
      const Type& ty_in = g_in->type();
      if (ty_in != Type::varsetint() && ty_in != Type::parsetint() && ty_in.dim() != 1) {
        throw TypeError(_env,g_in->loc(),
                        "generator expression must be (par or var) set of int or one-dimensional array, but is `"
                        +ty_in.toString(_env)+"'");
      }
      Type ty_id;
      bool needIntLit = false;
      if (ty_in.dim()==0) {
        ty_id = Type::parint();
        ty_id.enumId(ty_in.enumId());
        needIntLit = true;
      } else {
        ty_id = ty_in;
        if (ty_in.enumId() != 0) {
          const std::vector<unsigned int>& enumIds = _env.getArrayEnum(ty_in.enumId());
          ty_id.enumId(enumIds.back());
        }
        ty_id.dim(0);
      }
      for (int j=0; j<c.n_decls(gen_i); j++) {
        if (needIntLit) {
          GCLock lock;
          c.decl(gen_i,j)->e(IntLit::aEnum(0,ty_id.enumId()));
        }
        c.decl(gen_i,j)->type(ty_id);
        c.decl(gen_i,j)->ti()->type(ty_id);
      }
    }
    /// Visit if-then-else
    void vITE(ITE& ite) {
      Type tret = ite.e_else()->type();
      std::vector<AnonVar*> anons;
      bool allpar = !(tret.isvar());
      if (tret.isunknown()) {
        if (AnonVar* av = ite.e_else()->dyn_cast<AnonVar>()) {
          allpar = false;
          anons.push_back(av);
        } else {
          throw TypeError(_env,ite.e_else()->loc(), "cannot infer type of expression in else branch of conditional");
        }
      }
      bool allpresent = !(tret.isopt());
      bool varcond = false;
      for (int i=0; i<ite.size(); i++) {
        Expression* eif = ite.e_if(i);
        Expression* ethen = ite.e_then(i);
        varcond = varcond || (eif->type() == Type::varbool());
        if (eif->type() != Type::parbool() && eif->type() != Type::varbool())
          throw TypeError(_env,eif->loc(),
            "expected bool conditional expression, got `"+
            eif->type().toString(_env)+"'");
        if (eif->type().cv())
          tret.cv(true);
        if (ethen->type().isunknown()) {
          if (AnonVar* av = ethen->dyn_cast<AnonVar>()) {
            allpar = false;
            anons.push_back(av);
          } else {
            throw TypeError(_env,ethen->loc(), "cannot infer type of expression in then branch of conditional");
          }
        } else {
          if (tret.isbot() || tret.isunknown())
            tret.bt(ethen->type().bt());
          if ( (!ethen->type().isbot() && !Type::bt_subtype(ethen->type(), tret, true) && !Type::bt_subtype(tret, ethen->type(), true)) ||
              ethen->type().st() != tret.st() ||
              ethen->type().dim() != tret.dim()) {
            throw TypeError(_env,ethen->loc(),
                            "type mismatch in branches of conditional. Then-branch has type `"+
                            ethen->type().toString(_env)+"', but else branch has type `"+
                            tret.toString(_env)+"'");
          }
          if (Type::bt_subtype(tret, ethen->type(), true)) {
            tret.bt(ethen->type().bt());
          }
          if (ethen->type().isvar()) allpar=false;
          if (ethen->type().isopt()) allpresent=false;
          if (ethen->type().cv())
            tret.cv(true);
        }
      }
      Type tret_var(tret);
      tret_var.ti(Type::TI_VAR);
      for (unsigned int i=0; i<anons.size(); i++) {
        anons[i]->type(tret_var);
      }
      for (int i=0; i<ite.size(); i++) {
        ite.e_then(i, addCoercion(_env, _model,ite.e_then(i), tret)());
      }
      ite.e_else(addCoercion(_env, _model, ite.e_else(), tret)());
      /// TODO: perhaps extend flattener to array types, but for now throw an error
      if (varcond && tret.dim() > 0)
        throw TypeError(_env,ite.loc(), "conditional with var condition cannot have array type");
      if (varcond || !allpar)
        tret.ti(Type::TI_VAR);
      if (!allpresent)
        tret.ot(Type::OT_OPTIONAL);
      ite.type(tret);
    }
    /// Visit binary operator
    void vBinOp(BinOp& bop) {
      std::vector<Expression*> args(2);
      args[0] = bop.lhs(); args[1] = bop.rhs();
      if (FunctionI* fi = _model->matchFn(_env,bop.opToString(),args,true)) {
        bop.lhs(addCoercion(_env, _model,bop.lhs(),fi->argtype(_env,args, 0))());
        bop.rhs(addCoercion(_env, _model,bop.rhs(),fi->argtype(_env,args, 1))());
        args[0] = bop.lhs(); args[1] = bop.rhs();
        Type ty = fi->rtype(_env,args,true);
        ty.cv(bop.lhs()->type().cv() || bop.rhs()->type().cv());
        bop.type(ty);
        
        if (fi->e())
          bop.decl(fi);
        else
          bop.decl(NULL);
      } else {
        throw TypeError(_env,bop.loc(),
          std::string("type error in operator application for `")+
          bop.opToString().str()+"'. No matching operator found with left-hand side type `"
                        +bop.lhs()->type().toString(_env)+
                        "' and right-hand side type `"+bop.rhs()->type().toString(_env)+"'");
      }
    }
    /// Visit unary operator
    void vUnOp(UnOp& uop) {
      std::vector<Expression*> args(1);
      args[0] = uop.e();
      if (FunctionI* fi = _model->matchFn(_env,uop.opToString(),args,true)) {
        uop.e(addCoercion(_env, _model,uop.e(),fi->argtype(_env,args,0))());
        args[0] = uop.e();
        Type ty = fi->rtype(_env,args,true);
        ty.cv(uop.e()->type().cv());
        uop.type(ty);
        if (fi->e())
          uop.decl(fi);
      } else {
        throw TypeError(_env,uop.loc(),
          std::string("type error in operator application for `")+
          uop.opToString().str()+"'. No matching operator found with type `"+uop.e()->type().toString(_env)+"'");
      }
    }
    /// Visit call
    void vCall(Call& call) {
      std::vector<Expression*> args(call.args().size());
      std::copy(call.args().begin(),call.args().end(),args.begin());
      if (FunctionI* fi = _model->matchFn(_env,call.id(),args,true)) {
        bool cv = false;
        for (unsigned int i=0; i<args.size(); i++) {
          args[i] = addCoercion(_env, _model,call.args()[i],fi->argtype(_env,args,i))();
          call.args()[i] = args[i];
          cv = cv || args[i]->type().cv();
        }
        Type ty = fi->rtype(_env,args,true);
        ty.cv(cv);
        call.type(ty);
        call.decl(fi);
      } else {
        std::ostringstream oss;
        oss << "no function or predicate with this signature found: `";
        oss << call.id() << "(";
        for (unsigned int i=0; i<call.args().size(); i++) {
          oss << call.args()[i]->type().toString(_env);
          if (i<call.args().size()-1) oss << ",";
        }
        oss << ")'";
        throw TypeError(_env,call.loc(), oss.str());
      }
    }
    /// Visit let
    void vLet(Let& let) {
      bool cv = false;
      for (unsigned int i=0; i<let.let().size(); i++) {
        Expression* li = let.let()[i];
        cv = cv || li->type().cv();
        if (VarDecl* vdi = li->dyn_cast<VarDecl>()) {
          if (vdi->e()==NULL && vdi->type().is_set() && vdi->type().isvar() &&
              vdi->ti()->domain()==NULL) {
            _typeErrors.push_back(TypeError(_env,vdi->loc(),
                                            "set element type for `"+vdi->id()->str().str()+"' is not finite"));
          }
          if (vdi->type().ispar() && vdi->e() == NULL)
            throw TypeError(_env,vdi->loc(),
              "let variable `"+vdi->id()->v().str()+"' must be initialised");
          if (vdi->ti()->hasTiVariable()) {
            _typeErrors.push_back(TypeError(_env,vdi->loc(),
                                            "type-inst variables not allowed in type-inst for let variable `"+vdi->id()->str().str()+"'"));
          }
          let.let_orig()[i] = vdi->e();
        }
      }
      Type ty = let.in()->type();
      ty.cv(cv);
      let.type(ty);
    }
    /// Visit variable declaration
    void vVarDecl(VarDecl& vd) {
      if (ignoreVarDecl) {
        assert(!vd.type().isunknown());
        if (vd.e()) {
          Type vdt = vd.ti()->type();
          Type vet = vd.e()->type();
          if (vdt.enumId() != 0 && vdt.dim() > 0 &&
              (vd.e()->isa<ArrayLit>() || vd.e()->isa<Comprehension>() ||
               (vd.e()->isa<BinOp>() && vd.e()->cast<BinOp>()->op()==BOT_PLUSPLUS))) {
            // Special case: index sets of array literals and comprehensions automatically
            // coerce to any enum index set
            const std::vector<unsigned int>& enumIds = _env.getArrayEnum(vdt.enumId());
            if (enumIds[enumIds.size()-1]==0) {
              vdt.enumId(0);
            } else {
              std::vector<unsigned int> nEnumIds(enumIds.size());
              for (unsigned int i=0; i<nEnumIds.size()-1; i++)
                nEnumIds[i] = 0;
              nEnumIds[nEnumIds.size()-1] = enumIds[enumIds.size()-1];
              vdt.enumId(_env.registerArrayEnum(nEnumIds));
            }
          } else if (vd.ti()->isEnum() && vd.e()->isa<Call>()) {
            if (vd.e()->cast<Call>()->id()=="anon_enum") {
              vet.enumId(vdt.enumId());
            }
          }
          
          if (! _env.isSubtype(vet,vdt,true)) {
            _typeErrors.push_back(TypeError(_env,vd.e()->loc(),
                                            "initialisation value for `"+vd.id()->str().str()+"' has invalid type-inst: expected `"+
                                            vd.ti()->type().toString(_env)+"', actual `"+vd.e()->type().toString(_env)+"'"));
          } else {
            vd.e(addCoercion(_env, _model, vd.e(), vd.ti()->type())());
          }
        }
      } else {
        vd.type(vd.ti()->type());
        vd.id()->type(vd.type());
      }
    }
    /// Visit type inst
    void vTypeInst(TypeInst& ti) {
      Type tt = ti.type();
      bool foundEnum = ti.ranges().size()>0 && ti.domain() && ti.domain()->type().enumId() != 0;
      if (ti.ranges().size()>0) {
        bool foundTIId=false;
        for (unsigned int i=0; i<ti.ranges().size(); i++) {
          TypeInst* ri = ti.ranges()[i];
          assert(ri != NULL);
          if (ri->type().cv())
            tt.cv(true);
          if (ri->type().enumId() != 0) {
            foundEnum = true;
          }
          if (ri->type() == Type::top()) {
//            if (foundTIId) {
//              throw TypeError(_env,ri->loc(),
//                "only one type-inst variable allowed in array index");
//            } else {
              foundTIId = true;
//            }
          } else if (ri->type() != Type::parint()) {
            assert(ri->isa<TypeInst>());
            TypeInst* riti = ri->cast<TypeInst>();
            if (riti->domain()) {
              throw TypeError(_env,ri->loc(),
                "array index set expression has invalid type, expected `set of int', actual `set of "+
                ri->type().toString(_env)+"'");
            } else {
              throw TypeError(_env,ri->loc(),
                              "cannot use `"+ri->type().toString(_env)+"' as array index set (did you mean `int'?)");
            }
          }
        }
        tt.dim(foundTIId ? -1 : ti.ranges().size());
      }
      if (ti.domain() && ti.domain()->type().cv())
        tt.cv(true);
      if (ti.domain()) {
        if (TIId* tiid = ti.domain()->dyn_cast<TIId>()) {
          if (tiid->isEnum()) {
            tt.bt(Type::BT_INT);
          }
        } else {
          if (ti.domain()->type().ti() != Type::TI_PAR ||
              ti.domain()->type().st() != Type::ST_SET)
            throw TypeError(_env,ti.domain()->loc(),
                            "type-inst must be par set but is `"+ti.domain()->type().toString(_env)+"'");
          if (ti.domain()->type().dim() != 0)
            throw TypeError(_env,ti.domain()->loc(),
                            "type-inst cannot be an array");
        }
      }
      if (tt.isunknown()) {
        assert(ti.domain());
        switch (ti.domain()->type().bt()) {
        case Type::BT_INT:
        case Type::BT_FLOAT:
          break;
        case Type::BT_BOT:
          {
            Type tidt = ti.domain()->type();
            tidt.bt(Type::BT_INT);
            ti.domain()->type(tidt);
          }
          break;
        default:
          throw TypeError(_env,ti.domain()->loc(),
            "type-inst must be int or float");
        }
        tt.bt(ti.domain()->type().bt());
        tt.enumId(ti.domain()->type().enumId());
      } else {
//        assert(ti.domain()==NULL || ti.domain()->isa<TIId>());
      }
      if (foundEnum) {
        std::vector<unsigned int> enumIds(ti.ranges().size()+1);
        for (unsigned int i=0; i<ti.ranges().size(); i++) {
          enumIds[i] = ti.ranges()[i]->type().enumId();
        }
        enumIds[ti.ranges().size()] = ti.domain() ? ti.domain()->type().enumId() : 0;
        int arrayEnumId = _env.registerArrayEnum(enumIds);
        tt.enumId(arrayEnumId);
      }

      if (tt.st()==Type::ST_SET && tt.ti()==Type::TI_VAR && tt.bt() != Type::BT_INT && tt.bt() != Type::BT_TOP)
        throw TypeError(_env,ti.loc(), "var set element types other than `int' not allowed");
      ti.type(tt);
    }
    void vTIId(TIId& id) {}
  };
  
  void typecheck(Env& env, Model* m, std::vector<TypeError>& typeErrors, bool ignoreUndefinedParameters) {
    TopoSorter ts(m);
    
    std::vector<FunctionI*> functionItems;
    std::vector<AssignI*> assignItems;
    Model* enumItems = new Model;
    
    class TSV0 : public ItemVisitor {
    public:
      EnvI& env;
      TopoSorter& ts;
      Model* model;
      bool hadSolveItem;
      std::vector<FunctionI*>& fis;
      std::vector<AssignI*>& ais;
      Model* enumis;
      TSV0(EnvI& env0, TopoSorter& ts0, Model* model0, std::vector<FunctionI*>& fis0, std::vector<AssignI*>& ais0,
           Model* enumis0)
        : env(env0), ts(ts0), model(model0), hadSolveItem(false), fis(fis0), ais(ais0), enumis(enumis0) {}
      void vAssignI(AssignI* i) { ais.push_back(i); }
      void vVarDeclI(VarDeclI* i) { ts.add(env, i, true, enumis); }
      void vFunctionI(FunctionI* i) {
        model->registerFn(env, i);
        fis.push_back(i);
      }
      void vSolveI(SolveI* si) {
        if (hadSolveItem)
          throw TypeError(env,si->loc(),"Only one solve item allowed");
        hadSolveItem = true;
      }
    } _tsv0(env.envi(),ts,m,functionItems,assignItems,enumItems);
    iterItems(_tsv0,m);

    for (unsigned int i=0; i<enumItems->size(); i++) {
      if (AssignI* ai = (*enumItems)[i]->dyn_cast<AssignI>()) {
        assignItems.push_back(ai);
      } else if (VarDeclI* vdi = (*enumItems)[i]->dyn_cast<VarDeclI>()) {
        m->addItem(vdi);
        ts.add(env.envi(), vdi, false, enumItems);
      } else {
        FunctionI* fi = (*enumItems)[i]->dyn_cast<FunctionI>();
        m->addItem(fi);
        m->registerFn(env.envi(),fi);
        functionItems.push_back(fi);
      }
    }

    Model* enumItems2 = new Model;
    
    for (unsigned int i=0; i<assignItems.size(); i++) {
      AssignI* ai = assignItems[i];
      VarDecl* vd = ts.get(env.envi(),ai->id(),ai->loc());
      if (vd->e())
        throw TypeError(env.envi(),ai->loc(),"multiple assignment to the same variable");
      vd->e(ai->e());
      
      if (vd->ti()->isEnum()) {
        GCLock lock;
        ASTString name(createEnumToStringName(vd->id(),"_enum_to_string_"));
        VarDecl* vd_enum = ts.get(env.envi(),name,vd->loc());
        if (vd_enum->e())
          throw TypeError(env.envi(),ai->loc(),"multiple assignment to the same variable");
        AssignI* ai_enum = createEnumMapper(env.envi(), m, vd->ti()->type().enumId(), vd, vd_enum, enumItems2);
        if (ai_enum) {
          vd_enum->e(ai_enum->e());
          ai_enum->remove();
        }
      }
      ai->remove();
    }
    
    for (unsigned int i=0; i<enumItems2->size(); i++) {
      if (VarDeclI* vdi = (*enumItems2)[i]->dyn_cast<VarDeclI>()) {
        m->addItem(vdi);
        ts.add(env.envi(), vdi, false, enumItems);
      } else {
        FunctionI* fi = (*enumItems2)[i]->cast<FunctionI>();
        m->addItem(fi);
        m->registerFn(env.envi(),fi);
        functionItems.push_back(fi);
      }
    }
    
    delete enumItems;
    delete enumItems2;
    
    class TSV1 : public ItemVisitor {
    public:
      EnvI& env;
      TopoSorter& ts;
      TSV1(EnvI& env0, TopoSorter& ts0) : env(env0), ts(ts0) {}
      void vVarDeclI(VarDeclI* i) { ts.run(env,i->e()); }
      void vAssignI(AssignI* i) {}
      void vConstraintI(ConstraintI* i) { ts.run(env,i->e()); }
      void vSolveI(SolveI* i) {
        for (ExpressionSetIter it = i->ann().begin(); it != i->ann().end(); ++it)
          ts.run(env,*it);
        ts.run(env,i->e());
      }
      void vOutputI(OutputI* i) { ts.run(env,i->e()); }
      void vFunctionI(FunctionI* fi) {
        ts.run(env,fi->ti());
        for (unsigned int i=0; i<fi->params().size(); i++)
          ts.run(env,fi->params()[i]);
        for (ExpressionSetIter it = fi->ann().begin(); it != fi->ann().end(); ++it)
          ts.run(env,*it);
        ts.scopes.push(false);
        for (unsigned int i=0; i<fi->params().size(); i++)
          ts.scopes.add(env,fi->params()[i]);
        ts.run(env,fi->e());
        ts.scopes.pop();
      }
    } _tsv1(env.envi(),ts);
    iterItems(_tsv1,m);

    m->sortFn();

    {
      struct SortByPayload {
        bool operator ()(Item* i0, Item* i1) {
          if (i0->isa<IncludeI>())
            return !i1->isa<IncludeI>();
          if (VarDeclI* vdi0 = i0->dyn_cast<VarDeclI>()) {
            if (VarDeclI* vdi1 = i1->dyn_cast<VarDeclI>()) {
              return vdi0->e()->payload() < vdi1->e()->payload();
            } else {
              return !i1->isa<IncludeI>();
            }
          }
          return false;
        }
      } _sbp;
      
      std::stable_sort(m->begin(), m->end(), _sbp);
    }

    
    {
      Typer<false> ty(env.envi(), m, typeErrors);
      BottomUpIterator<Typer<false> > bu_ty(ty);
      for (unsigned int i=0; i<ts.decls.size(); i++) {
        ts.decls[i]->payload(0);
        bu_ty.run(ts.decls[i]->ti());
        ty.vVarDecl(*ts.decls[i]);
      }
      for (unsigned int i=0; i<functionItems.size(); i++) {
        bu_ty.run(functionItems[i]->ti());
        for (unsigned int j=0; j<functionItems[i]->params().size(); j++)
          bu_ty.run(functionItems[i]->params()[j]);
      }
    }
    
    {
      Typer<true> ty(env.envi(), m, typeErrors);
      BottomUpIterator<Typer<true> > bu_ty(ty);
      
      class TSV2 : public ItemVisitor {
      public:
        EnvI& env;
        Model* m;
        BottomUpIterator<Typer<true> >& bu_ty;
        std::vector<TypeError>& _typeErrors;
        TSV2(EnvI& env0, Model* m0,
             BottomUpIterator<Typer<true> >& b,
             std::vector<TypeError>& typeErrors) : env(env0), m(m0), bu_ty(b), _typeErrors(typeErrors) {}
        void vVarDeclI(VarDeclI* i) {
          bu_ty.run(i->e());
          if (i->e()->ti()->hasTiVariable()) {
            _typeErrors.push_back(TypeError(env, i->e()->loc(),
                                            "type-inst variables not allowed in type-inst for `"+i->e()->id()->str().str()+"'"));
          }
          VarDecl* vdi = i->e();
          if (vdi->e()==NULL && vdi->type().is_set() && vdi->type().isvar() &&
              vdi->ti()->domain()==NULL) {
            _typeErrors.push_back(TypeError(env,vdi->loc(),
                                            "set element type for `"+vdi->id()->str().str()+"' is not finite"));
          }
        }
        void vAssignI(AssignI* i) {
          bu_ty.run(i->e());
          if (!env.isSubtype(i->e()->type(),i->decl()->ti()->type(),true)) {
            _typeErrors.push_back(TypeError(env, i->e()->loc(),
                                           "assignment value for `"+i->decl()->id()->str().str()+"' has invalid type-inst: expected `"+
                                           i->decl()->ti()->type().toString(env)+"', actual `"+i->e()->type().toString(env)+"'"));
            // Assign to "true" constant to avoid generating further errors that the parameter
            // is undefined
            i->decl()->e(constants().lit_true);
          }
        }
        void vConstraintI(ConstraintI* i) {
          bu_ty.run(i->e());
          if (!env.isSubtype(i->e()->type(),Type::varbool(),true))
            throw TypeError(env, i->e()->loc(), "invalid type of constraint, expected `"
                            +Type::varbool().toString(env)+"', actual `"+i->e()->type().toString(env)+"'");
        }
        void vSolveI(SolveI* i) {
          for (ExpressionSetIter it = i->ann().begin(); it != i->ann().end(); ++it) {
            bu_ty.run(*it);
            if (!(*it)->type().isann())
              throw TypeError(env, (*it)->loc(), "expected annotation, got `"+(*it)->type().toString(env)+"'");
          }
          bu_ty.run(i->e());
          if (i->e()) {
            Type et = i->e()->type();

            bool needOptCoercion = et.isopt() && et.isint();
            if (needOptCoercion) {
              et.ot(Type::OT_PRESENT);
            }
            
            if (! (env.isSubtype(et,Type::varint(),true) ||
                   env.isSubtype(et,Type::varfloat(),true)))
              throw TypeError(env, i->e()->loc(),
                "objective has invalid type, expected int or float, actual `"+et.toString(env)+"'");

            if (needOptCoercion) {
              GCLock lock;
              std::vector<Expression*> args(2);
              args[0] = i->e();
              args[1] = constants().boollit(i->st()==SolveI::ST_MAX);
              Call* c = new Call(Location().introduce(), ASTString("objective_deopt_"), args);
              c->decl(env.orig->matchFn(env, c, false));
              assert(c->decl());
              c->type(et);
              i->e(c);
            }
          }
        }
        void vOutputI(OutputI* i) {
          bu_ty.run(i->e());
          if (i->e()->type() != Type::parstring(1) && i->e()->type() != Type::bot(1))
            throw TypeError(env, i->e()->loc(), "invalid type in output item, expected `"
                            +Type::parstring(1).toString(env)+"', actual `"+i->e()->type().toString(env)+"'");
        }
        void vFunctionI(FunctionI* i) {
          for (ExpressionSetIter it = i->ann().begin(); it != i->ann().end(); ++it) {
            bu_ty.run(*it);
            if (!(*it)->type().isann())
              throw TypeError(env, (*it)->loc(), "expected annotation, got `"+(*it)->type().toString(env)+"'");
          }
          bu_ty.run(i->ti());
          bu_ty.run(i->e());
          if (i->e() && !env.isSubtype(i->e()->type(),i->ti()->type(),true))
            throw TypeError(env, i->e()->loc(), "return type of function does not match body, declared type is `"
                            +i->ti()->type().toString(env)+
                            "', body type is `"+i->e()->type().toString(env)+"'");
          if (i->e())
            i->e(addCoercion(env, m, i->e(), i->ti()->type())());
        }
      } _tsv2(env.envi(), m, bu_ty, typeErrors);
      iterItems(_tsv2,m);
    }
    
    class TSV3 : public ItemVisitor {
    public:
      EnvI& env;
      Model* m;
      OutputI* outputItem;
      TSV3(EnvI& env0, Model* m0) : env(env0), m(m0), outputItem(NULL) {}
      void vAssignI(AssignI* i) {
        i->decl()->e(addCoercion(env, m, i->e(), i->decl()->type())());
      }
      void vOutputI(OutputI* oi) {
        if (outputItem==NULL) {
          outputItem = oi;
        } else {
          GCLock lock;
          BinOp* bo = new BinOp(Location().introduce(),outputItem->e(),BOT_PLUSPLUS,oi->e());
          bo->type(Type::parstring(1));
          outputItem->e(bo);
          oi->remove();
          m->setOutputItem(outputItem);
        }
      }
    } _tsv3(env.envi(),m);
    if (typeErrors.empty()) {
      iterItems(_tsv3,m);
    }

    try {
      m->checkFnOverloading(env.envi());
    } catch (TypeError& e) {
      typeErrors.push_back(e);
    }
    
    for (unsigned int i=0; i<ts.decls.size(); i++) {
      if (ts.decls[i]->toplevel() &&
          ts.decls[i]->type().ispar() && !ts.decls[i]->type().isann() && ts.decls[i]->e()==NULL) {
        if (ts.decls[i]->type().isopt()) {
          ts.decls[i]->e(constants().absent);
        } else if (!ignoreUndefinedParameters) {
          typeErrors.push_back(TypeError(env.envi(), ts.decls[i]->loc(),
                                         "  symbol error: variable `" + ts.decls[i]->id()->str().str()
                                         + "' must be defined (did you forget to specify a data file?)"));
        }
      }
    }

  }
  
  void typecheck(Env& env, Model* m, AssignI* ai) {
    std::vector<TypeError> typeErrors;
    Typer<true> ty(env.envi(), m, typeErrors);
    BottomUpIterator<Typer<true> > bu_ty(ty);
    bu_ty.run(ai->e());
    if (!typeErrors.empty()) {
      throw typeErrors[0];
    }
    if (!env.envi().isSubtype(ai->e()->type(),ai->decl()->ti()->type(),true)) {
      throw TypeError(env.envi(), ai->e()->loc(),
                      "assignment value for `"+ai->decl()->id()->str().str()+"' has invalid type-inst: expected `"+
                      ai->decl()->ti()->type().toString(env.envi())+"', actual `"+ai->e()->type().toString(env.envi())+"'");
    }
    
  }

  void typecheck_fzn(Env& env, Model* m) {
    ASTStringMap<int>::t declMap;
    for (unsigned int i=0; i<m->size(); i++) {
      if (VarDeclI* vdi = (*m)[i]->dyn_cast<VarDeclI>()) {
        Type t = vdi->e()->type();
        declMap.insert(std::pair<ASTString,int>(vdi->e()->id()->v(), i));
        if (t.isunknown()) {
          if (vdi->e()->ti()->domain()) {
            switch (vdi->e()->ti()->domain()->eid()) {
              case Expression::E_BINOP:
              {
                BinOp* bo = vdi->e()->ti()->domain()->cast<BinOp>();
                if (bo->op()==BOT_DOTDOT) {
                  t.bt(bo->lhs()->type().bt());
                  if (t.isunknown()) {
                    throw TypeError(env.envi(), vdi->e()->loc(), "Cannot determine type of variable declaration");
                  }
                  vdi->e()->type(t);
                } else {
                  throw TypeError(env.envi(), vdi->e()->loc(), "Only ranges allowed in FlatZinc type inst");
                }
              }
              case Expression::E_ID:
              {
                ASTStringMap<int>::t::iterator it = declMap.find(vdi->e()->ti()->domain()->cast<Id>()->v());
                if (it == declMap.end()) {
                  throw TypeError(env.envi(), vdi->e()->loc(), "Cannot determine type of variable declaration");
                }
                t.bt((*m)[it->second]->cast<VarDeclI>()->e()->type().bt());
                if (t.isunknown()) {
                  throw TypeError(env.envi(), vdi->e()->loc(), "Cannot determine type of variable declaration");
                }
                vdi->e()->type(t);
              }
              default:
                throw TypeError(env.envi(), vdi->e()->loc(), "Cannot determine type of variable declaration");
            }
          } else {
            throw TypeError(env.envi(), vdi->e()->loc(), "Cannot determine type of variable declaration");
          }
        }
      }
    }
  }

  void output_var_desc_json(Env& env, VarDecl* vd, std::ostream& os) {
    os << "    \"" << *vd->id() << "\" : {";
    os << "\"type\" : ";
    switch (vd->type().bt()) {
      case Type::BT_INT: os << "\"int\""; break;
      case Type::BT_BOOL: os << "\"bool\""; break;
      case Type::BT_FLOAT: os << "\"float\""; break;
      case Type::BT_STRING: os << "\"string\""; break;
      case Type::BT_ANN: os << "\"ann\""; break;
      default: os << "\"?\""; break;
    }
    if (vd->type().ot()==Type::OT_OPTIONAL) {
      os << ", \"optional\" : true";
    }
    if (vd->type().st()==Type::ST_SET) {
      os << ", \"set\" : true";
    }
    if (vd->type().dim() > 0) {
      os << ", \"dim\" : " << vd->type().dim();
    }
    os << "}";
  }
  
  void output_model_interface(Env& env, Model* m, std::ostream& os) {
    std::ostringstream oss_input;
    std::ostringstream oss_output;
    bool had_input = false;
    bool had_output = false;
    for (VarDeclIterator vds = m->begin_vardecls(); vds != m->end_vardecls(); ++vds) {
      if (vds->e()->type().ispar() && (vds->e()->e()==NULL || vds->e()->e()==constants().absent)) {
        if (had_input) oss_input << ",\n";
        output_var_desc_json(env, vds->e(), oss_input);
        had_input = true;
      } else if (vds->e()->type().isvar() && (vds->e()->e()==NULL || vds->e()->ann().contains(constants().ann.add_to_output))) {
        if (had_output) oss_output << ",\n";
        output_var_desc_json(env, vds->e(), oss_output);
        had_output = true;
      }
    }
    os << "{\n  \"input\" : {\n" << oss_input.str() << "\n  },\n  \"output\" : {\n" << oss_output.str() << "\n  }";
    os << ",\n  \"method\": \"";
    if (m->solveItem()) {
      switch (m->solveItem()->st()) {
        case SolveI::ST_MIN: os << "min"; break;
        case SolveI::ST_MAX: os << "max"; break;
        case SolveI::ST_SAT: os << "sat"; break;
      }
    }
    os << "\"";
    os << "\n}\n";
  }
  
}
