/*
 * 05_sparse_matrix.cpp — sparse matrix: DOK builder (std::map) -> CSR, SpMV, and
 *                        sparse x sparse multiply (Gustavson's algorithm)
 *
 * Concept : Build in a format that is easy to modify (dictionary of keys:
 *           map<(row,col), value>, O(log nnz) insert), then freeze into CSR
 *           for fast arithmetic. Gustavson's SpGEMM computes C = A*B one
 *           row at a time with a dense "accumulator" row and a list of
 *           touched columns, so work is proportional to the useful flops,
 *           not rows*cols*inner.
 * Build   : make run P=dsa/19-system-design-structures/cpp/05_sparse_matrix.cpp
 * Read    : ../README.md  (section "Sparse matrix")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <utility>
#include <vector>

// Dictionary Of Keys: ordered by (row, col), so iteration is already in CSR order.
class DokMatrix {
public:
    DokMatrix(int r, int c) : rows_(r), cols_(c) {}
    void add(int r, int c, double v) {        // accumulate (duplicates sum up)
        assert(r >= 0 && r < rows_ && c >= 0 && c < cols_);
        double& x = data_[{r, c}];
        x += v;
        if (x == 0.0) data_.erase({r, c});    // keep the structure truly sparse
    }
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    const std::map<std::pair<int, int>, double>& data() const { return data_; }
private:
    int rows_, cols_;
    std::map<std::pair<int, int>, double> data_;
};

class CsrMatrix {
public:
    explicit CsrMatrix(const DokMatrix& d) : rows_(d.rows()), cols_(d.cols()), row_ptr_(static_cast<std::size_t>(d.rows()) + 1, 0) {
        for (const auto& [rc, v] : d.data()) {             // O(nnz): already sorted
            ++row_ptr_[static_cast<std::size_t>(rc.first) + 1];
            col_.push_back(rc.second);
            val_.push_back(v);
        }
        for (std::size_t r = 0; r < static_cast<std::size_t>(rows_); ++r) row_ptr_[r + 1] += row_ptr_[r];
    }
    CsrMatrix(int r, int c) : rows_(r), cols_(c), row_ptr_(static_cast<std::size_t>(r) + 1, 0) {}

    std::size_t nnz() const { return val_.size(); }

    // y = A x, O(nnz)
    std::vector<double> operator*(const std::vector<double>& x) const {
        assert(static_cast<int>(x.size()) == cols_);
        std::vector<double> y(static_cast<std::size_t>(rows_), 0.0);
        for (int r = 0; r < rows_; ++r)
            for (int k = row_ptr_[r]; k < row_ptr_[r + 1]; ++k)
                y[static_cast<std::size_t>(r)] += val_[k] * x[static_cast<std::size_t>(col_[k])];
        return y;
    }

    // C = A * B (Gustavson). For each row i of A, for each a_ik, add a_ik * row k of B
    // into a dense accumulator; `touched` remembers which columns are non-zero so
    // resetting the accumulator costs O(row nnz), not O(cols).
    friend CsrMatrix operator*(const CsrMatrix& A, const CsrMatrix& B) {
        assert(A.cols_ == B.rows_);
        CsrMatrix C(A.rows_, B.cols_);
        std::vector<double> acc(static_cast<std::size_t>(B.cols_), 0.0);
        std::vector<char> used(static_cast<std::size_t>(B.cols_), 0);
        std::vector<int> touched;
        for (int i = 0; i < A.rows_; ++i) {
            touched.clear();
            for (int ka = A.row_ptr_[i]; ka < A.row_ptr_[i + 1]; ++ka) {
                int k = A.col_[ka];
                for (int kb = B.row_ptr_[k]; kb < B.row_ptr_[k + 1]; ++kb) {
                    auto j = static_cast<std::size_t>(B.col_[kb]);
                    if (!used[j]) { used[j] = 1; touched.push_back(B.col_[kb]); }
                    acc[j] += A.val_[ka] * B.val_[kb];
                }
            }
            std::sort(touched.begin(), touched.end());   // CSR wants sorted columns
            for (int j : touched) {
                auto uj = static_cast<std::size_t>(j);
                if (acc[uj] != 0.0) { C.col_.push_back(j); C.val_.push_back(acc[uj]); }
                acc[uj] = 0.0;
                used[uj] = 0;
            }
            C.row_ptr_[static_cast<std::size_t>(i) + 1] = static_cast<int>(C.val_.size());
        }
        return C;
    }

    std::vector<std::vector<double>> to_dense() const {
        std::vector<std::vector<double>> d(static_cast<std::size_t>(rows_), std::vector<double>(static_cast<std::size_t>(cols_), 0.0));
        for (int r = 0; r < rows_; ++r)
            for (int k = row_ptr_[r]; k < row_ptr_[r + 1]; ++k)
                d[static_cast<std::size_t>(r)][static_cast<std::size_t>(col_[k])] = val_[k];
        return d;
    }

private:
    int rows_, cols_;
    std::vector<int> row_ptr_;
    std::vector<int> col_;
    std::vector<double> val_;
};

using Dense = std::vector<std::vector<double>>;

static Dense dense_mul(const Dense& a, const Dense& b) {
    std::size_t n = a.size(), m = b[0].size(), inner = b.size();
    Dense c(n, std::vector<double>(m, 0.0));
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t k = 0; k < inner; ++k)
            for (std::size_t j = 0; j < m; ++j) c[i][j] += a[i][k] * b[k][j];
    return c;
}

static DokMatrix random_dok(std::mt19937& rng, int r, int c, int n, Dense& mirror) {
    DokMatrix d(r, c);
    mirror.assign(static_cast<std::size_t>(r), std::vector<double>(static_cast<std::size_t>(c), 0.0));
    for (int i = 0; i < n; ++i) {
        int rr = static_cast<int>(rng() % static_cast<unsigned>(r));
        int cc = static_cast<int>(rng() % static_cast<unsigned>(c));
        double v = static_cast<double>(static_cast<int>(rng() % 9) - 4);   // small ints: exact
        d.add(rr, cc, v);
        mirror[static_cast<std::size_t>(rr)][static_cast<std::size_t>(cc)] += v;
    }
    return d;
}

int main() {
    std::cout << "== graph adjacency as a sparse matrix: count 2-hop paths with A*A ==\n";
    // 0->1, 0->2, 1->3, 2->3, 3->0
    DokMatrix g(4, 4);
    for (auto [u, v] : std::vector<std::pair<int, int>>{{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 0}}) g.add(u, v, 1);
    CsrMatrix A(g);
    auto A2 = (A * A).to_dense();
    std::cout << "paths of length 2 from 0 to 3: " << A2[0][3] << "\n";
    assert(A2[0][3] == 2.0);   // 0->1->3 and 0->2->3
    assert(A2[3][1] == 1.0 && A2[3][2] == 1.0);

    std::cout << "\n== random matrices vs dense reference ==\n";
    std::mt19937 rng(2024);
    for (int trial = 0; trial < 40; ++trial) {
        int n = 1 + static_cast<int>(rng() % 30), k = 1 + static_cast<int>(rng() % 30), m = 1 + static_cast<int>(rng() % 30);
        Dense da, db;
        CsrMatrix sa(random_dok(rng, n, k, static_cast<int>(rng() % static_cast<unsigned>(n * k / 5 + 2)), da));
        CsrMatrix sb(random_dok(rng, k, m, static_cast<int>(rng() % static_cast<unsigned>(k * m / 5 + 2)), db));
        assert(sa.to_dense() == da);                        // exact: small integers

        std::vector<double> x(static_cast<std::size_t>(k));
        for (auto& xi : x) xi = static_cast<double>(static_cast<int>(rng() % 5) - 2);
        auto y = sa * x;
        for (std::size_t i = 0; i < da.size(); ++i) {
            double s = 0;
            for (std::size_t j = 0; j < x.size(); ++j) s += da[i][j] * x[j];
            assert(s == y[i]);
        }
        assert((sa * sb).to_dense() == dense_mul(da, db));
    }
    std::cout << "40 trials: DOK->CSR, SpMV and SpGEMM all match dense\n";
    std::cout << "OK\n";
    return 0;
}
