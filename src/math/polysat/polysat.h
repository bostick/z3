/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    polysat

Abstract:

    Polynomial solver for modular arithmetic.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19

--*/
#pragma once

#include "util/dependency.h"
#include "util/trail.h"
#include "util/lbool.h"
#include "util/rlimit.h"
#include "util/scoped_ptr_vector.h"
#include "util/var_queue.h"
#include "util/ref_vector.h"
#include "math/dd/dd_pdd.h"
#include "math/dd/dd_bdd.h"

namespace polysat {

    class solver;
    typedef dd::pdd pdd;
    typedef dd::bdd bdd;
    typedef unsigned pvar;

    const unsigned null_dependency = UINT_MAX;
    const pvar null_var = UINT_MAX;

    struct dep_value_manager {
        void inc_ref(unsigned) {}
        void dec_ref(unsigned) {}
    };

    struct dep_config {
        typedef dep_value_manager value_manager;
        typedef unsigned value;
        typedef small_object_allocator allocator;
        static const bool ref_count = false;
    };

    typedef dependency_manager<dep_config> poly_dep_manager;
    typedef poly_dep_manager::dependency p_dependency;

    typedef obj_ref<p_dependency, poly_dep_manager> p_dependency_ref; 
    typedef ref_vector<p_dependency, poly_dep_manager> p_dependency_refv;

    enum ckind_t { eq_t, ule_t, sle_t };

    class constraint {
        unsigned         m_level;
        ckind_t          m_kind;
        pdd              m_poly;
        pdd              m_other;
        p_dependency_ref m_dep;
        unsigned_vector  m_vars;
        constraint(unsigned lvl, pdd const& p, pdd const& q, p_dependency_ref& dep, ckind_t k): 
            m_level(lvl), m_kind(k), m_poly(p), m_other(q), m_dep(dep) {
            m_vars.append(p.free_vars());
            if (q != p) 
                for (auto v : q.free_vars())
                    m_vars.insert(v);            
            }
    public:
        static constraint* eq(unsigned lvl, pdd const& p, p_dependency_ref& d) { 
            return alloc(constraint, lvl, p, p, d, ckind_t::eq_t); 
        }
        static constraint* ule(unsigned lvl, pdd const& p, pdd const& q, p_dependency_ref& d) { 
            return alloc(constraint, lvl, p, q, d, ckind_t::ule_t); 
        }
        bool is_eq() const { return m_kind == ckind_t::eq_t; }
        bool is_ule() const { return m_kind == ckind_t::ule_t; }
        bool is_sle() const { return m_kind == ckind_t::sle_t; }
        ckind_t kind() const { return m_kind; }
        pdd const &  p() const { return m_poly; }
        pdd const &  lhs() const { return m_poly; }
        pdd const &  rhs() const { return m_other; }
        std::ostream& display(std::ostream& out) const;
        p_dependency* dep() const { return m_dep; }
        unsigned_vector& vars() { return m_vars; }
        unsigned level() const { return m_level; }
    };

    inline std::ostream& operator<<(std::ostream& out, constraint const& c) { return c.display(out); }


    /**
     * Justification kind for a variable assignment.
     */
    enum justification_k { unassigned, decision, propagation };

    class justification {
        justification_k m_kind;
        unsigned        m_level;
        justification(justification_k k, unsigned lvl): m_kind(k), m_level(lvl) {}
    public:
        justification(): m_kind(justification_k::unassigned) {}
        static justification unassigned() { return justification(justification_k::unassigned, 0); }
        static justification decision(unsigned lvl) { return justification(justification_k::decision, lvl); }
        static justification propagation(unsigned lvl) { return justification(justification_k::propagation, lvl); }
        bool is_decision() const { return m_kind == justification_k::decision; }
        bool is_unassigned() const { return m_kind == justification_k::unassigned; }
        bool is_propagation() const { return m_kind == justification_k::propagation; }
        justification_k kind() const { return m_kind; }
        unsigned level() const { return m_level; }
        std::ostream& display(std::ostream& out) const;
    };

    inline std::ostream& operator<<(std::ostream& out, justification const& j) { return j.display(out); }

    class solver {

        typedef ptr_vector<constraint> constraints;

        trail_stack&             m_trail;
        reslimit&                m_lim;
        scoped_ptr_vector<dd::pdd_manager> m_pdd;
        dd::bdd_manager          m_bdd;
        dep_value_manager        m_value_manager;
        small_object_allocator   m_alloc;
        poly_dep_manager         m_dm;
        constraints              m_conflict;
        constraints              m_stash_just;
        var_queue                m_free_vars;

        // Per constraint state
        scoped_ptr_vector<constraint>   m_constraints;
        scoped_ptr_vector<constraint>   m_redundant;

        // Per variable information
        vector<bdd>              m_viable;   // set of viable values.
        vector<rational>         m_value;    // assigned value
        vector<justification>    m_justification; // justification for variable assignment
        vector<constraints>      m_cjust;    // constraints justifying variable range.
        vector<constraints>      m_watch;    // watch list datastructure into constraints.
        unsigned_vector          m_activity; 
        vector<pdd>              m_vars;
        unsigned_vector          m_size;     // store size of variables.

        // search state that lists assigned variables
        vector<std::pair<pvar, rational>> m_search;

        unsigned                 m_qhead { 0 };
        unsigned                 m_level { 0 };

        unsigned_vector          m_base_levels;  // External clients can push/pop scope. 


        unsigned size(pvar v) const { return m_size[v]; }
        /**
         * check if value is viable according to m_viable.
         */
        bool is_viable(pvar v, rational const& val);

        /**
         * register that val is non-viable for var.
         */
        void add_non_viable(pvar v, rational const& val);

        /**
         * Add dependency for variable viable range.
         */
        void add_viable_dep(pvar v, p_dependency* dep);

        
        /**
         * Find a next viable value for varible.
         * l_false - there are no viable values.
         * l_true  - there is only one viable value left.
         * l_undef - there are multiple viable values, return a guess
         */
        lbool find_viable(pvar v, rational & val);

        /**
         * undo trail operations for backtracking.
         * Each struct is a subclass of trail and implements undo().
         */
        struct t_del_var;

        void del_var();

        dd::pdd_manager& sz2pdd(unsigned sz);

        void push_level();
        void pop_levels(unsigned num_levels);
        void pop_assignment();
        void pop_constraints(scoped_ptr_vector<constraint>& cs);

        void assign_core(pvar v, rational const& val, justification const& j);

        bool is_assigned(pvar v) const { return !m_justification[v].is_unassigned(); }

        void propagate(pvar v);
        bool propagate(pvar v, constraint& c);
        bool propagate_eq(pvar v, constraint& c);
        void propagate(pvar v, rational const& val, constraint& c);
        void erase_watch(pvar v, constraint& c);
        void erase_watch(constraint& c);
        void add_watch(constraint& c);

        void set_conflict(constraint& c);
        void set_conflict(pvar v);

        unsigned_vector m_marks;
        unsigned        m_clock { 0 };
        void reset_marks();
        bool is_marked(pvar v) const { return m_clock == m_marks[v]; }
        void set_mark(pvar v) { m_marks[v] = m_clock; }

        unsigned                 m_conflict_level { 0 };

        constraint* resolve(pvar v);

        bool can_decide() const { return !m_free_vars.empty(); }
        void decide();
        void decide(pvar v);

        p_dependency* mk_dep(unsigned dep) { return dep == null_dependency ? nullptr : m_dm.mk_leaf(dep); }

        bool is_conflict() const { return !m_conflict.empty(); }
        bool at_base_level() const;
        unsigned base_level() const;

        void resolve_conflict();            
        void backtrack(unsigned i, scoped_ptr<constraint>& lemma);
        void report_unsat();
        void revert_decision(pvar v);
        void learn_lemma(pvar v, constraint* c);
        void backjump(unsigned new_level);
        void undo_var(pvar v);
        void add_lemma(constraint* c);
        bool is_always_false(constraint& c);
        bool eval_to_false(constraint& c);



        bool invariant();
        bool invariant(scoped_ptr_vector<constraint> const& cs);

    public:

        /**
         * to share chronology we pass an external trail stack.
         * every update to the solver is going to be retractable
         * by pushing an undo action on the trail stack.
         */
        solver(trail_stack& s, reslimit& lim);

        ~solver();

        /**
         * End-game satisfiability checker.
         */
        lbool check_sat();

        /**
         * retrieve unsat core dependencies
         */
        void unsat_core(unsigned_vector& deps);
        
        /**
         * Add variable with bit-size. 
         */
        pvar add_var(unsigned sz);

        /**
         * Create polynomial terms
         */
        pdd var(pvar v) { return m_vars[v]; }

        /**
         * Add polynomial constraints
         * Each constraint is tracked by a dependency.
         * assign sets the 'index'th bit of var.
         */
        void add_eq(pdd const& p, unsigned dep = null_dependency);
        void add_diseq(pdd const& p, unsigned dep = null_dependency);
        void add_ule(pdd const& p, pdd const& q, unsigned dep = null_dependency);
        void add_ult(pdd const& p, pdd const& q, unsigned dep = null_dependency);
        void add_sle(pdd const& p, pdd const& q, unsigned dep = null_dependency);
        void add_slt(pdd const& p, pdd const& q, unsigned dep = null_dependency);
        

        /**
         * main state transitions.
         */
        bool can_propagate();
        void propagate();

        /**
         * External context managment.
         * Adds so-called user-scope.
         */
        void push();
        void pop(unsigned num_scopes);
       
        std::ostream& display(std::ostream& out) const;

    };

    inline std::ostream& operator<<(std::ostream& out, solver const& s) { return s.display(out); }

}


