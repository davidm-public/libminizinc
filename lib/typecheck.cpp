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
#include <minizinc/exception.hh>

#include <string>
#include <sstream>

namespace MiniZinc {
  
  struct VarDeclCmp {
    std::unordered_map<VarDecl*,int>& _pos;
    VarDeclCmp(std::unordered_map<VarDecl*,int>& pos) : _pos(pos) {}
    bool operator()(Expression* e0, Expression* e1) {
      if (VarDecl* vd0 = e0->dyn_cast<VarDecl>()) {
        if (VarDecl* vd1 = e1->dyn_cast<VarDecl>()) {
          return _pos.at(vd0) < _pos.at(vd1);
        } else {
          return true;
        }
      } else {
        return false;
      }
    }
  };
  struct ItemCmp {
    std::unordered_map<VarDecl*,int>& _pos;
    ItemCmp(std::unordered_map<VarDecl*,int>& pos) : _pos(pos) {}
    bool operator()(Item* i0, Item* i1) {
      if (VarDeclI* vd0 = i0->cast<VarDeclI>()) {
        if (VarDeclI* vd1 = i1->cast<VarDeclI>()) {
          return _pos.at(vd0->_e) < _pos.at(vd1->_e);
        } else {
          return true;
        }
      } else {
        return false;
      }
    }
  };
  
  class TopoSorter {
  public:
    typedef std::vector<VarDecl*> Decls;
    typedef CtxStringMap<Decls>::t DeclMap;
    typedef std::unordered_map<VarDecl*,int> PosMap;
    
    Decls decls;
    DeclMap env;
    PosMap pos;
    
    void add(VarDecl* vd, bool unique) {
      DeclMap::iterator vdi = env.find(vd->_id);
      if (vdi == env.end()) {
        Decls nd; nd.push_back(vd);
        env.insert(std::pair<CtxStringH,Decls>(vd->_id,nd));
      } else {
        if (unique)
          throw TypeError(vd->_loc,"identifier `"+vd->_id.str()+
                          "' already defined");
        vdi->second.push_back(vd);
      }
    }
    void remove(VarDecl* vd) {
      DeclMap::iterator vdi = env.find(vd->_id);
      assert(vdi != env.end());
      vdi->second.pop_back();
      if (vdi->second.empty())
        env.erase(vdi);
    }
    
    VarDecl* checkId(const CtxStringH& id, const Location& loc) {
      DeclMap::iterator decl = env.find(id);
      if (decl==env.end()) {
        throw TypeError(loc,"undefined identifier "+id.str());
      }
      PosMap::iterator pi = pos.find(decl->second.back());
      if (pi==pos.end()) {
        // new id
        run(decl->second.back());
      } else {
        // previously seen, check if circular
        if (pi->second==-1)
          throw TypeError(loc,"circular definition of "+id.str());
      }
      return decl->second.back();
    }
    
    void run(Expression* e) {
      if (e==NULL)
        return;
      if (e->_ann)
        run(e->_ann);
      switch (e->_eid) {
      case Expression::E_INTLIT:
      case Expression::E_FLOATLIT:
      case Expression::E_BOOLLIT:
      case Expression::E_STRINGLIT:
      case Expression::E_ANON:
        break;
      case Expression::E_SETLIT:
        {
          SetLit* sl = e->cast<SetLit>();
          for (Expression* ei : *sl->_v)
            run(ei);
        }
        break;
      case Expression::E_ID:
        {
          e->cast<Id>()->_decl = checkId(e->cast<Id>()->_v,e->_loc);
        }
        break;
      case Expression::E_ARRAYLIT:
        {
          ArrayLit* al = e->cast<ArrayLit>();
          for (Expression* ei : *al->_v)
            run(ei);
        }
        break;
      case Expression::E_ARRAYACCESS:
        {
          ArrayAccess* ae = e->cast<ArrayAccess>();
          run(ae->_v);
          for (Expression* ei : *ae->_idx)
            run(ei);
        }
        break;
      case Expression::E_COMP:
        {
          Comprehension* ce = e->cast<Comprehension>();
          for (Generator* g : *ce->_g)
            for (VarDecl* vd : *g->_v)
              add(vd,false);
          for (Generator* g : *ce->_g)
            run(g->_in);
          if (ce->_where)
            run(ce->_where);
          run(ce->_e);
          for (Generator* g : *ce->_g)
            for (VarDecl* vd : *g->_v)
              remove(vd);
        }
        break;
      case Expression::E_ITE:
        {
          ITE* ite = e->cast<ITE>();
          for (ITE::IfThen& ie : *ite->_e_if) {
            run(ie.first);
            run(ie.second);
          }
          run(ite->_e_else);
        }
        break;
      case Expression::E_BINOP:
        {
          BinOp* be = e->cast<BinOp>();
          run(be->_e0);
          run(be->_e1);
        }
        break;
      case Expression::E_UNOP:
        {
          UnOp* ue = e->cast<UnOp>();
          run(ue->_e0);
        }
        break;
      case Expression::E_CALL:
        {
          Call* ce = e->cast<Call>();
          for (Expression* ei : *ce->_args)
            run(ei);
        }
        break;
      case Expression::E_VARDECL:
        {
          VarDecl* ve = e->cast<VarDecl>();
          PosMap::iterator pi = pos.find(ve);
          if (pi==pos.end()) {
            pos.insert(std::pair<VarDecl*,int>(ve,-1));
            run(ve->_ti);
            run(ve->_e);
            decls.push_back(ve);
            pi = pos.find(ve);
            pi->second = decls.size()-1;
          } else {
            assert(pi->second != -1);
          }
        }
        break;
      case Expression::E_ANN:
        {
          Annotation* ann = e->cast<Annotation>();
          run(ann->_e);
          run(ann->_a);
        }
        break;
      case Expression::E_TI:
        {
          TypeInst* ti = e->cast<TypeInst>();
          if (ti->_ranges)
            for (Expression* ei : *ti->_ranges)
              run(ei);
          run(ti->_domain);
        }
        break;
      case Expression::E_TIID:
        break;
      case Expression::E_LET:
        {
          Let* let = e->cast<Let>();
          for (Expression* ei : *let->_let) {
            if (VarDecl* vd = ei->dyn_cast<VarDecl>()) {
              add(vd,false);
            }
          }
          for (Expression* ei : *let->_let) {
            run(ei);
          }
          run(let->_in);
          VarDeclCmp poscmp(pos);
          std::stable_sort(let->_let->begin(), let->_let->end(), poscmp);
          for (Expression* ei : *let->_let) {
            if (VarDecl* vd = ei->dyn_cast<VarDecl>()) {
              remove(vd);
            }
          }
        }
        break;
      }
    }
  };
  
  template<bool ignoreVarDecl>
  class Typer {
  public:
    static const bool visitAnnotation = ignoreVarDecl;
    ASTContext& _ctx;
    Typer(ASTContext& ctx) : _ctx(ctx) {}
    /// Visit integer literal
    void vIntLit(const IntLit&) {}
    /// Visit floating point literal
    void vFloatLit(const FloatLit&) {}
    /// Visit Boolean literal
    void vBoolLit(const BoolLit&) {}
    /// Visit set literal
    void vSetLit(SetLit& sl) {
      Type ty; ty._st = Type::ST_SET;
      for (Expression* ei : *sl._v) {
        if (ei->_type.isvar())
          ty._ti = Type::TI_VAR;
        if (ty._bt!=ei->_type._bt) {
          if (ty._bt!=Type::BT_UNKNOWN)
            throw TypeError(sl._loc,"non-uniform set literal");
          ty._bt = ei->_type._bt;
        }
      }
      if (ty._bt == Type::BT_UNKNOWN)
        ty._bt = Type::BT_BOT;
      sl._type = ty;
    }
    /// Visit string literal
    void vStringLit(const StringLit&) {}
    /// Visit identifier
    void vId(Id& id) {
      assert(!id._decl->_type.isunknown());
      id._type = id._decl->_type;
    }
    /// Visit anonymous variable
    void vAnonVar(const AnonVar&) {}
    /// Visit array literal
    void vArrayLit(ArrayLit& al) {
      Type ty; ty._dim = al._dims->size();
      for (Expression* vi : *al._v) {
        if (vi->_type.isvar() || vi->_type.isany())
          ty._ti = Type::TI_VAR;
        if (ty._bt==Type::BT_UNKNOWN) {
          ty._bt = vi->_type._bt;
          assert(ty._bt != Type::BT_UNKNOWN);
          ty._st = vi->_type._st;
        } else if (ty._bt != vi->_type._bt ||
                   ty._st != vi->_type._st) {
          throw TypeError(al._loc,"non-uniform array literal");
        }
      }
      if (ty._bt == Type::BT_UNKNOWN)
        ty._bt = Type::BT_BOT;
      al._type = ty;
    }
    /// Visit array access
    void vArrayAccess(ArrayAccess& aa) {
      if (aa._v->_type._dim==0)
        throw TypeError(aa._v->_loc,"not an array in array access");
      if (aa._v->_type._dim != aa._idx->size())
        throw TypeError(aa._v->_loc,"array dimensions do not match");
      bool allpar=true;
      for (Expression* aai : *aa._idx) {
        if (aai->_type==Type::varint() || aai->_type==Type::any()) {
          allpar=false;
        } else if (aai->_type!=Type::parint()) {
          throw TypeError(aai->_loc,"array index must be int");
        }
      }
      aa._type = aa._v->_type;
      aa._type._dim = 0;
      if (!allpar)
        aa._type._ti = Type::TI_VAR;
    }
    /// Visit array comprehension
    void vComprehension(Comprehension& c) {
      for (Generator* g : *c._g) {
        const Type& ty_in = g->_in->_type;
        if (ty_in != Type::parsetint() && !ty_in.isintarray()) {
          throw TypeError(g->_in->_loc,
            "generator expression must be par set of int or array[int] of int, but is "+ty_in.toString());
        }
      }
      if (c._where && c._where->_type != Type::parbool()) {
        throw TypeError(c._where->_loc,
                        "where clause must be par bool, but is "+
                        c._where->_type.toString());
        
      }
      c._type = c._e->_type;
      if (c._set) {
        if (c._e->_type._dim != 0 || c._e->_type._st == Type::ST_SET)
          throw TypeError(c._e->_loc,
              "set comprehension expression must be scalar, but is "
              +c._e->_type.toString());
        c._type._st = Type::ST_SET;
      } else {
        if (c._e->_type._dim != 0)
          throw TypeError(c._e->_loc,
            "array comprehension expression cannot be an array");
        c._type._dim = 1;
      }
    }
    /// Visit if-then-else
    void vITE(ITE& ite) {
      Type& telse = ite._e_else->_type;
      bool allpar = !(telse.isvar());
      for (ITE::IfThen& it : *ite._e_if) {
        Expression* eif = it.first;
        Expression* ethen = it.second;
        if (eif->_type != Type::parbool())
          throw TypeError(eif->_loc,
            "expected par bool conditional expression, got\n  "+
            eif->_type.toString());
        if (ethen->_type._bt != telse._bt ||
            ethen->_type._st != telse._st ||
            ethen->_type._dim != telse._dim) {
          throw TypeError(ethen->_loc,
            "type mismatch in branches of conditional. Then-branch has type "+
            ethen->_type.toString()+", but else branch has type "+
            telse.toString());
        }
        if (ethen->_type.isvar()) allpar=false;
      }
      ite._type = telse;
      if (!allpar) ite._type._ti = Type::TI_VAR;
    }
    /// Visit binary operator
    void vBinOp(BinOp& bop) {
      std::vector<Expression*> args(2);
      args[0] = bop._e0; args[1] = bop._e1;
      if (FunctionI* fi = _ctx.matchFn(bop.opToString(),args)) {
        bop._type = fi->rtype(args);
      } else if (bop._op==BOT_PLUSPLUS &&
        bop._e0->_type._dim==1 && bop._e1->_type._dim==1 &&
        bop._e0->_type._st==bop._e1->_type._st &&
        bop._e0->_type._bt==bop._e1->_type._bt) {
        if (bop._e0->_type.isvar())
          bop._type = bop._e0->_type;
        else
          bop._type = bop._e1->_type;
      } else {
        throw TypeError(bop._loc,
          std::string("type error in operator application for ")+
          bop.opToString().str());
      }
    }
    /// Visit unary operator
    void vUnOp(UnOp& uop) {
      std::vector<Expression*> args(1);
      args[0] = uop._e0;
      if (FunctionI* fi = _ctx.matchFn(uop.opToString(),args)) {
        uop._type = fi->rtype(args);
      } else {
        throw TypeError(uop._loc,
          std::string("type error in operator application for ")+
          uop.opToString().str());
      }
    }
    /// Visit call
    void vCall(Call& call) {
      std::vector<Expression*> args(call._args->size());
      std::copy(call._args->begin(),call._args->end(),args.begin());
      if (FunctionI* fi = _ctx.matchFn(call._id,args)) {
        call._type = fi->rtype(args);
        call._decl = fi;
      } else {
        throw TypeError(call._loc,
          "no function or predicate with this signature found");
      }
    }
    /// Visit let
    void vLet(Let& let) {
      for (Expression* li : *let._let) {
        if (VarDecl* vdi = li->dyn_cast<VarDecl>()) {
          if (vdi->_type.ispar() && vdi->_e == NULL)
            throw TypeError(vdi->_loc,
              "let variable `"+vdi->_id.str()+"' must be defined");
        }
      }
      let._type = let._in->_type;
    }
    /// Visit annotation
    void vAnnotation(Annotation& ann) {}
    /// Visit variable declaration
    void vVarDecl(VarDecl& vd) {
      if (ignoreVarDecl) {
        assert(!vd._type.isunknown());
        if (vd._e) {
          if (! vd._e->_type.isSubtypeOf(vd._ti->_type))
            throw TypeError(vd._loc,
              "type error in initialization, LHS is\n  "+
              vd._ti->_type.toString()+"\nbut RHS is\n  "+
              vd._e->_type.toString());
        }
      } else {
        vd._type = vd._ti->_type;
      }
    }
    /// Visit type inst
    void vTypeInst(TypeInst& ti) {
      if (ti._ranges) {
        bool foundTIId=false;
        for (TypeInst* ri : *ti._ranges) {
          assert(ri != NULL);
          if (ri->_type == Type::bot()) {
//            std::cerr << "tiid " << ri->cast<TIId>()->_v.str() << "\n";
            if (foundTIId) {
              throw TypeError(ri->_loc,
                "only one type-inst variable allowed in array index");
            } else {
              foundTIId = true;
            }
          } else if (ri->_type != Type::parint()) {
            assert(ri->isa<TypeInst>());
            std::cerr << "expected set of int for array index, but got " <<
              ri->_type.toString() << "\n";
            assert(false);
            throw TypeError(ri->_loc,
              "expected set of int for array index, but got\n"+
              ri->_type.toString());
          }
        }
        ti._type._dim = foundTIId ? -1 : ti._ranges->size();
      }
      if (ti._domain && !ti._domain->isa<TIId>()) {
        if (ti._domain->_type._ti != Type::TI_PAR ||
            ti._domain->_type._st != Type::ST_SET)
          throw TypeError(ti._domain->_loc,
            "type-inst must be par set");
        if (ti._domain->_type._dim != 0)
          throw TypeError(ti._domain->_loc,
            "type-inst cannot be an array");
      }
      if (ti._type.isunknown()) {
        assert(ti._domain);
        switch (ti._domain->_type._bt) {
        case Type::BT_INT:
        case Type::BT_FLOAT:
          break;
        case Type::BT_BOT:
          ti._domain->_type._bt = Type::BT_INT;
          break;
        default:
          throw TypeError(ti._domain->_loc,
            "type-inst must be int or float");
        }
        ti._type._bt = ti._domain->_type._bt;
      } else {
//        assert(ti._domain==NULL || ti._domain->isa<TIId>());
      }
    }
    void vTIId(TIId& id) {}
  };
  
  void typecheck(ASTContext& ctx, Model* m) {
    TopoSorter ts;
    
    std::vector<FunctionI*> functionItems;
    
    std::vector<Model*> models;
    models.push_back(m);
    while (!models.empty()) {
      Model* cm = models.back();
      models.pop_back();
      // Register types of all declared functions
      for (Item* it : cm->_items) {
        switch (it->_iid) {
        case Item::II_INC:
          if (it->cast<IncludeI>()->_own)
            models.push_back(it->cast<IncludeI>()->_m);
          break;
        case Item::II_VD:
          ts.add(it->cast<VarDeclI>()->_e,true);
          break;
        case Item::II_ASN:
          break;
        case Item::II_CON:
          break;
        case Item::II_SOL:
          break;
        case Item::II_OUT:
          break;
        case Item::II_FUN:
          ctx.registerFn(it->cast<FunctionI>());
          functionItems.push_back(it->cast<FunctionI>());
          break;      
        }
      }
    }
    
    models.push_back(m);
    while (!models.empty()) {
      Model* cm = models.back();
      models.pop_back();
      // Run toposort for all expressions
      for (Item* it : cm->_items) {
        switch (it->_iid) {
        case Item::II_INC:
          if (it->cast<IncludeI>()->_own)
            models.push_back(it->cast<IncludeI>()->_m);
          break;
        case Item::II_VD:
          ts.run(it->cast<VarDeclI>()->_e);
          break;
        case Item::II_ASN:
          {
            AssignI* ai = it->cast<AssignI>();
            ts.run(ai->_e);
            ai->_decl = ts.checkId(ai->_id,ai->_loc);
          }
          break;
        case Item::II_CON:
          ts.run(it->cast<ConstraintI>()->_e);
          break;
        case Item::II_SOL:
          ts.run(it->cast<SolveI>()->_ann);
          ts.run(it->cast<SolveI>()->_e);
          break;
        case Item::II_OUT:
          ts.run(it->cast<OutputI>()->_e);
          break;
        case Item::II_FUN:
          {
            FunctionI* fi = it->cast<FunctionI>();
            ts.run(fi->_ti);
            for (unsigned int i=0; i<fi->_params->size(); i++)
              ts.run((*fi->_params)[i]);
            ts.run(fi->_ann);
            for (unsigned int i=0; i<fi->_params->size(); i++)
              ts.add((*fi->_params)[i],false);
            ts.run(fi->_e);
            for (unsigned int i=0; i<fi->_params->size(); i++)
              ts.remove((*fi->_params)[i]);
          }
          break;      
        }
      }
    }

    ctx.sortFn();

    {
      Typer<false> ty(ctx);
      BottomUpIterator<Typer<false> > bu_ty(ty);
      for (VarDecl* vd : ts.decls)
        bu_ty.run(vd);
      for (FunctionI* fi : functionItems) {
        bu_ty.run(fi->_ti);
        for (VarDecl* vd : *fi->_params)
          bu_ty.run(vd);
      }
    }
    
    {
      Typer<true> ty(ctx);
      BottomUpIterator<Typer<true> > bu_ty(ty);
      models.push_back(m);
      while (!models.empty()) {
        Model* cm = models.back();
        models.pop_back();
        // Run type checking for all expressions
        for (Item* it : cm->_items) {
          switch (it->_iid) {
          case Item::II_INC:
            if (it->cast<IncludeI>()->_own)
              models.push_back(it->cast<IncludeI>()->_m);
            break;
          case Item::II_VD:
            bu_ty.run(it->cast<VarDeclI>()->_e);
            break;
          case Item::II_ASN:
            {
              AssignI* ai = it->cast<AssignI>();
              bu_ty.run(ai->_e);
              if (!ai->_e->_type.isSubtypeOf(ai->_decl->_ti->_type)) {
                throw TypeError(ai->_e->_loc,
                  "RHS of assignment does not agree with LHS");
              }
            }
            break;
          case Item::II_CON:
            bu_ty.run(it->cast<ConstraintI>()->_e);
            if (!it->cast<ConstraintI>()->
              _e->_type.isSubtypeOf(Type::varbool()))
              throw TypeError(it->cast<ConstraintI>()->_e->_loc,
                "constraint must be var bool");
            break;
          case Item::II_SOL:
            {
              bu_ty.run(it->cast<SolveI>()->_ann);
              bu_ty.run(it->cast<SolveI>()->_e);
              if (it->cast<SolveI>()->_e) {
                Type et = it->cast<SolveI>()->_e->_type;
                if (! (et.isSubtypeOf(Type::varint()) || 
                       et.isSubtypeOf(Type::varfloat())))
                  throw TypeError(it->cast<SolveI>()->_e->_loc,
                    "objective must be int or float");
              }
            }
            break;
          case Item::II_OUT:
            bu_ty.run(it->cast<OutputI>()->_e);
            if (it->cast<OutputI>()->_e->_type != Type::parstring(1))
              throw TypeError(it->cast<OutputI>()->_e->_loc,
                "output item needs string array");
            break;
          case Item::II_FUN:
            {
              FunctionI* fi = it->cast<FunctionI>();
              bu_ty.run(fi->_ann);
              bu_ty.run(fi->_e);
            }
            break;      
          }
        }
      }
    }
    
  }
  
}