/*
 * 05_smart_pointers.cpp — unique_ptr, shared_ptr, weak_ptr in depth, and the reference-cycle leak
 *
 * Concept : unique_ptr = sole owner, same size as a raw pointer, move-only.
 *           shared_ptr = pointer + pointer to a CONTROL BLOCK holding an atomic
 *           strong count and weak count; the object dies when strong hits 0,
 *           the control block when weak also hits 0.
 *           A CYCLE of shared_ptrs (A owns B, B owns A) never reaches 0: leak.
 *           Make one direction weak_ptr to break it.
 * Build   : make run P=dsa/02-memory-management/cpp/05_smart_pointers.cpp
 * Read    : ../README.md  (section "3.13 Smart pointers")
 */
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static int g_alive = 0;

struct Node {
    std::string name;
    std::shared_ptr<Node> partner;     // OWNING link
    std::weak_ptr<Node> parent;        // NON-owning link
    explicit Node(std::string n) : name(std::move(n)) { ++g_alive; }
    ~Node() { --g_alive; std::cout << "    ~Node(" << name << ")\n"; }
};

// Factory returning ownership: the caller decides what to do with it.
static std::unique_ptr<Node> make_node(const char *n) { return std::make_unique<Node>(n); }

int main()
{
    std::cout << "1. unique_ptr\n";
    {
        std::unique_ptr<Node> a = make_node("A");
        static_assert(sizeof(a) == sizeof(Node *), "zero overhead");
        std::unique_ptr<Node> b = std::move(a);       // transfer; a is now null
        assert(!a && b->name == "A");
        std::shared_ptr<Node> s = std::move(b);       // unique -> shared is allowed
        assert(s.use_count() == 1);

        auto arr = std::make_unique<int[]>(8);        // array form: uses delete[]
        arr[7] = 1;

        // Custom deleter: any C resource.
        auto closer = [](FILE *f) { if (f) std::fclose(f); };
        std::unique_ptr<FILE, decltype(closer)> f(std::fopen("/dev/null", "r"), closer);
        assert(f);

        Node *raw = make_node("R").release();         // give up ownership: YOU must delete
        delete raw;
    }
    assert(g_alive == 0);

    std::cout << "2. shared_ptr and the control block\n";
    {
        /*
         *   s1 ─┐                        heap (make_shared: ONE allocation)
         *       ├──► +-----------------+--------------------+
         *   s2 ─┘    | control block   | Node "S"           |
         *            | strong = 2      |                    |
         *            | weak   = 1 (+w) |                    |
         *            +-----------------+--------------------+
         *   w  ─────► (same control block; does not keep Node alive)
         */
        auto s1 = std::make_shared<Node>("S");
        auto s2 = s1;
        std::weak_ptr<Node> w = s1;
        assert(s1.use_count() == 2 && !w.expired());
        s1.reset();
        assert(s2.use_count() == 1 && g_alive == 1);
        s2.reset();                                    // strong -> 0: Node destroyed
        assert(w.expired() && g_alive == 0 && w.lock() == nullptr);

        // Aliasing constructor: share ownership of the Node, point at a member.
        auto owner = std::make_shared<Node>("Alias");
        std::shared_ptr<std::string> name_ptr(owner, &owner->name);
        owner.reset();
        assert(*name_ptr == "Alias" && g_alive == 1);  // still alive through name_ptr
    }
    assert(g_alive == 0);

    std::cout << "3. reference cycle -> leak\n";
    std::weak_ptr<Node> spy;                            // observe without owning
    {
        auto a = std::make_shared<Node>("cycleA");
        auto b = std::make_shared<Node>("cycleB");
        a->partner = b;                                 // A owns B
        b->partner = a;                                 // B owns A  -> cycle
        spy = a;
        assert(a.use_count() == 2 && b.use_count() == 2);
    }   // a and b (the locals) die: counts drop 2 -> 1, never 0
    std::cout << "    after scope: alive = " << g_alive << " (LEAKED: no destructor ran)\n";
    assert(g_alive == 2 && !spy.expired());
    //  ┌─────────┐ partner ┌─────────┐
    //  │ cycleA  │────────►│ cycleB  │   each keeps the other's strong count at 1;
    //  │ strong 1│◄────────│ strong 1│   nothing outside points in -> unreachable
    //  └─────────┘ partner └─────────┘   but never freed. (LeakSanitizer would report it.)
    // Clean up so this demo itself does not leak: break the cycle by hand.
    if (auto a = spy.lock())
        a->partner->partner.reset();                    // B lets go of A
    assert(g_alive == 0 && spy.expired());

    std::cout << "4. break cycles with weak_ptr (parent <- child)\n";
    {
        auto parent = std::make_shared<Node>("parent");
        auto child = std::make_shared<Node>("child");
        parent->partner = child;                        // parent OWNS child
        child->parent = parent;                         // child only OBSERVES parent
        assert(parent.use_count() == 1);
        if (auto p = child->parent.lock())
            assert(p->name == "parent");
    }
    assert(g_alive == 0);
    std::cout << "    no leak: both destroyed\n";

    std::cout << "5. vector of unique_ptr: polymorphic ownership without manual delete\n";
    {
        std::vector<std::unique_ptr<Node>> nodes;
        for (int i = 0; i < 3; i++) nodes.push_back(std::make_unique<Node>("v" + std::to_string(i)));
        assert(g_alive == 3);
    }
    assert(g_alive == 0);

    std::cout << "All checks passed.\n";
    return 0;
}
