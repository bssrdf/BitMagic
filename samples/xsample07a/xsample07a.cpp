/*
Copyright(c) 2002-2020 Anatoliy Kuznetsov(anatoliy_kuznetsov at yahoo.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

For more information please visit:  http://bitmagic.io
*/

/** \example xsample07a.cpp
    Use of bvector<> for k-mer fingerprint K should be short,
    no minimizers here
*/

/*! \file xsample07a.cpp
    \brief Example: Use of bvector<> for k-mer fingerprint
    K should be short, no minimizers here
*/

#include <assert.h>
#include <stdlib.h>

#include <iostream>
#include <vector>
#include <map>
#include <algorithm>
#include <utility>
#include <memory>

#include <future>
#include <thread>
#include <mutex>
#include <atomic>

#include "bm64.h"  // use 48-bit vectors
#include "bmalgo.h"
#include "bmserial.h"
#include "bmaggregator.h"
#include "bmsparsevec_compr.h"
#include "bmsparsevec_algo.h"
#include "bmrandom.h"


// BitMagic utilities for debug and timings
#include "bmdbg.h"
#include "bmtimer.h"

#include "dna_finger.h"

using namespace std;



// Arguments
//
std::string  ifa_name;
std::string  ikd_name;
std::string  ikd_counts_name;
std::string  kh_name;
std::string  ikd_rep_name;
std::string  ikd_freq_name;
bool         is_diag = false;
bool         is_timing = false;
bool         is_bench = false;
unsigned     ik_size = 8;
unsigned     parallel_jobs = 4;
unsigned     f_percent = 5; // percent of k-mers we try to clear as over-represented

#include "cmd_args.h"



// Global types
//
typedef std::vector<char>                             vector_char_type;
typedef bm::dynamic_heap_matrix<unsigned, bm::bvector<>::allocator_type> distance_matrix_type;
typedef DNA_FingerprintScanner<bm::bvector<> >        dna_scanner_type;
typedef bm::sparse_vector<unsigned, bm::bvector<> >   sparse_vector_u32;
typedef bm::rsc_sparse_vector<unsigned, sparse_vector_u32 > rsc_sparse_vector_u32;
typedef std::map<unsigned, unsigned>                  histogram_map_u32;


// Global vars
//
bm::chrono_taker::duration_map_type     timing_map;
//dna_scanner_type                        dna_scanner;

class CSequenceColl
{
public:
    typedef std::vector<unsigned char> buffer_type;
public:

    CSequenceColl()
    {}
    CSequenceColl(const CSequenceColl&) = delete;

    void add_sequence(const string& acc, vector_char_type* seq_ptr)
    {
        m_acc.push_back(acc);
        m_seqs.emplace_back(seq_ptr);
    }

    void set_buffer(size_t i, const buffer_type& buf)
    {
        unique_ptr<buffer_type> buf_ptr(new buffer_type(buf));
        {
            static std::mutex   mtx_counts_lock;
            std::lock_guard<std::mutex> guard(mtx_counts_lock);

            if (m_kmer_bufs.size() <= i)
                m_kmer_bufs.resize(i+1);
            m_kmer_bufs[i].reset(buf_ptr.release());
        }
    }
    void sync_buffers_size()
    {
        m_kmer_bufs.resize(this->size());
    }

    size_t size() const
        { assert(m_seqs.size() == m_acc.size()); return m_seqs.size(); }

    const string& get_acc(size_t i) const { return m_acc[i]; }
    const vector_char_type& get_sequence(size_t i) const { return *(m_seqs[i]); }

    size_t seq_size(size_t i) const { return m_seqs[i]->size(); }

    size_t total_seq_size() const
    {
        size_t sum = 0;
        for (size_t i = 0; i < m_seqs.size(); ++i)
            sum += seq_size(i);
        return sum;
    }

    ///
    size_t buf_size() const { return m_kmer_bufs.size(); }

    /// Get k-mer vector BLOB size
    size_t get_buf_size(size_t i) const { return m_kmer_bufs[i]->size(); }

    /// Get k-mer BLOB pointer
    const unsigned char* get_buf(size_t i) const
    {
        const buffer_type* p =  m_kmer_bufs[i].get();
        if (!p)
            return 0;
        return p->data();
    }


private:
    vector<unique_ptr<vector_char_type> > m_seqs;
    vector<string>                        m_acc;
    vector<unique_ptr<buffer_type> >      m_kmer_bufs;
};


static
int load_FASTA(const std::string& fname, CSequenceColl& seq_coll)
{
    unique_ptr<vector_char_type> seq_vect(new vector_char_type());
    std::string line, acc;

    std::ifstream fin(fname.c_str(), std::ios::in);
    if (!fin.good())
        return -1;
    for (size_t i = 0; std::getline(fin, line); ++i)
    {
        if (line.empty())
            continue;

        if (line.front() == '>') // defline
        {
            if (!acc.empty())
            {
                seq_vect->shrink_to_fit();
                seq_coll.add_sequence(acc, seq_vect.release());
                acc.resize(0);
                seq_vect.reset(new vector_char_type());
            }

            std::size_t pos = line.find_first_of(":");
            if (pos == std::string::npos) // not found
            {
                acc = line;
            }
            else
            {
                acc = line.substr(1, pos-1);
            }
            continue;
        }
        for (std::string::iterator it = line.begin(); it != line.end(); ++it)
            seq_vect->push_back(*it);
    } // for

    if (!acc.empty())
    {
        seq_vect->shrink_to_fit();
        seq_coll.add_sequence(acc, seq_vect.release());
    }

    cout << "\r                            \r" << endl;
    return 0;
}

/// save k-mer vectors to a file
static
void save_kmer_buffers(const std::string& fname, const CSequenceColl& seq_coll)
{
    char magic_ch = '\t';
    std::ofstream bfile (fname, std::ios::out | std::ios::binary);
    if (!bfile.good())
    {
        std::cerr << "Cannot open file for write: " << fname << std::endl;
        exit(1);
    }

    // save collection size
    size_t sz = seq_coll.size();
    bfile.write((char*)&sz, std::streamsize(sizeof(sz)));

    // save the collection elements
    //
    for (size_t i = 0; i < sz; ++i)
    {
        size_t buf_size = 0;
        const unsigned char* buf = seq_coll.get_buf(i);
        if (!buf)
        {
            bfile.write((char*)&buf_size, std::streamsize(sizeof(buf_size)));
            continue;
        }
        buf_size = seq_coll.get_buf_size(i);
        bfile.write((char*)&buf_size, std::streamsize(sizeof(buf_size)));
        if (buf_size)
        {
            bfile.write((char*)buf, std::streamsize(buf_size));
            bfile.write((char*)&magic_ch, 1);
        }
    } // for i
}

/// Load k-mer vectors
///
static
void load_kmer_buffers(const std::string& fname, CSequenceColl& seq_coll)
{
    char magic_ch = '\t';
    std::ifstream bfile (fname, std::ios::in | std::ios::binary);
    if (!bfile.good())
    {
        std::cerr << "Cannot open file for read: " << fname << std::endl;
        exit(1);
    }

    // save collection size
    size_t sz;
    bfile.read((char*)&sz, std::streamsize(sizeof(sz)));

    CSequenceColl::buffer_type buf;

    // load the collection elements
    //
    for (size_t i = 0; i < sz; ++i)
    {
        size_t buf_size = 0;
        bfile.read((char*)&buf_size, std::streamsize(sizeof(buf_size)));
        if (buf_size)
        {
            buf.resize(buf_size);
            bfile.read((char*) buf.data(), std::streamsize(buf_size));
            char control_ch = 0;
            bfile.read((char*)&control_ch, 1);
            if (control_ch != magic_ch)
            {
                cerr << "Control read failure!" << endl;
                exit(1);
            }
            seq_coll.set_buffer(i, buf);
        }

    } // for i
}



inline
bool get_DNA_code(char bp, bm::id64_t& dna_code)
{
    switch (bp)
    {
    case 'A':
        dna_code = 0; // 00
        break;
    case 'T':
        dna_code = 1; // 01
        break;
    case 'G':
        dna_code = 2; // 10
        break;
    case 'C':
        dna_code = 3; // 11
        break;
    default: // ambiguity codes are ignored (for simplicity)
        return false;
    }
    return true;
}

/// Calculate k-mer as an unsigned long integer
///
///
/// @return true - if k-mer is "true" (not 'NNNNNN')
///
inline
bool get_kmer_code(const char* dna,
                  size_t pos, unsigned k_size,
                  bm::id64_t& k_mer)
{
    // generate k-mer
    //
    bm::id64_t k_acc = 0;
    unsigned shift = 0;
    dna += pos;
    for (size_t i = 0; i < k_size; ++i)
    {
        char bp = dna[i];
        bm::id64_t dna_code;
        bool valid = get_DNA_code(bp, dna_code);
        if (!valid)
            return false;
        k_acc |= (dna_code << shift); // accumulate new code within 64-bit accum
        shift += 2; // each DNA base pair needs 2-bits to store
    } // for i
    k_mer = k_acc;
    return true;
}


/// Translate integer code to DNA letter
///
inline
char int2DNA(unsigned code)
{
    static char lut[] = { 'A', 'T', 'G', 'C', 'N', 'M', '$' };
    if (code < 5)
        return lut[code];
    assert(0);
    return 'N';
}

/// Translate k-mer code into ATGC DNA string
///
/// @param dna    - target string
/// @param k_mer  - k-mer code
/// @param k_size -
inline
void translate_kmer(std::string& dna, bm::id64_t kmer_code, unsigned k_size)
{
    dna.resize(k_size);
    for (size_t i = 0; i < k_size; ++i)
    {
        unsigned dna_code = unsigned(kmer_code & 3);
        char bp = int2DNA(dna_code);
        dna[i] = bp;
        kmer_code >>= 2;
    } // for i
    assert(!kmer_code);
}



/**
    This function turns each k-mer into an integer number and encodes it
    in a bit-vector (presense vector)
    The natural limitation here is that integer has to be less tha 48-bits
    (limitations of bm::bvector<>)
    This method build a presense k-mer fingerprint vector which can be
    used for Jaccard distance comparison.

    @param bv - [out] - target bit-vector
    @param seq_vect - [out] DNA sequence vector
    @param k-size   - dimention for k-mer generation
    @param k_buf    - sort buffer for generated k-mers
    @param chunk_size - sort buffer size (number of k-mers per sort)
 */
template<typename BV>
void generate_k_mer_bvector(BV& bv,
                            const vector_char_type& seq_vect,
                            unsigned k_size,
                            std::vector<bm::id64_t>& k_buf,
                            const bm::id64_t chunk_size = 400000000
                            )
{
    bv.clear();
    bv.init(); // need to explicitly init to use bvector<>::set_bit_no_check()
    if (seq_vect.empty())
        return;
    const char* dna_str = &seq_vect[0];

    k_buf.reserve(chunk_size);
    k_buf.resize(0);

    {
        bm::id64_t k_mer_code;
        vector_char_type::size_type dna_sz = seq_vect.size()-(k_size-1);
        vector_char_type::size_type pos = 0;
        bool valid = false;
        for (; pos < dna_sz; ++pos)
        {
            valid = get_kmer_code(dna_str, pos, k_size, k_mer_code);
            if (valid)
            {
                k_buf.push_back(k_mer_code);
                break;
            }
        } // for pos

        const unsigned k_shift = (k_size-1) * 2;
        if (valid)
        {
            for (++pos; pos < dna_sz; ++pos)
            {
                bm::id64_t bp_code;
                valid = get_DNA_code(dna_str[pos + (k_size - 1)],  bp_code);
                if (!valid)
                {
                    pos += k_size; // wind fwrd to the next BP char
                    for (; pos < dna_sz; ++pos) // search for the next valid k-mer
                    {
                        valid = get_kmer_code(dna_str, pos, k_size, k_mer_code);
                        if (valid)
                        {
                            k_buf.push_back(k_mer_code);
                            break;
                        }
                    }
                    continue;
                }
                // shift out the previous base pair code, OR the new arrival
                k_mer_code = ((k_mer_code >> 2) | (bp_code << k_shift));
                // generated k-mer codes are accumulated in buffer for sorting
                k_buf.push_back(k_mer_code);

                if (k_buf.size() == chunk_size) // soring check.point
                {
                    std::sort(k_buf.begin(), k_buf.end());
                    if (k_buf.size())
                    {
                        bv.set(&k_buf[0], k_buf.size(), bm::BM_SORTED); // fast bulk set
                        k_buf.resize(0);
                        bv.optimize(); // periodically re-optimize to save memory
                    }

                    float pcnt = float(pos) / float(dna_sz);
                    pcnt *= 100;
                    cout << "\r" << unsigned(pcnt) << "% of " << dna_sz
                         << " (" << (pos+1) <<")    "
                         << flush;
                }
            } // for pos
        }

        if (k_buf.size()) // add last incomplete chunk here
        {
            std::sort(k_buf.begin(), k_buf.end());
            bv.set(&k_buf[0], k_buf.size(), bm::BM_SORTED); // fast bulk set
        }
    }
}

std::atomic_ullong                      k_mer_progress_count(0);

static
void generate_k_mers(CSequenceColl& seq_coll, unsigned k_size,
                     size_t from, size_t to)
{
    assert(from <= to);
    if (!seq_coll.size() || (from >= seq_coll.size()))
        return;

    std::vector<bm::id64_t> k_buf; // sort buffer
    BM_DECLARE_TEMP_BLOCK(tb)

    CSequenceColl::buffer_type buf;
    typedef bm::bvector<>::allocator_type        allocator_type;
    typedef allocator_type::allocator_pool_type  allocator_pool_type;
    allocator_pool_type  pool; // local pool for blocks

    bm::bvector<> bv;
    bm::bvector<>::mem_pool_guard mp_guard_bv; // memory pool reduces allocation calls to heap
    mp_guard_bv.assign_if_not_set(pool, bv);

    if (!to || to >= seq_coll.size())
        to = seq_coll.size()-1;

    bm::serializer<bm::bvector<> > bvs; // serializer object
    bvs.set_bookmarks(false);

    for (size_t i = from; i <= to; ++i)
    {
        const vector_char_type& seq_vect = seq_coll.get_sequence(i);
        generate_k_mer_bvector(bv, seq_vect, k_size, k_buf);

        // serialize the vector
        //
        typename bm::bvector<>::statistics st;
        bv.optimize(tb, bm::bvector<>::opt_compress, &st);

        buf.resize(st.max_serialize_mem);

        size_t blob_size = bvs.serialize(bv, &buf[0], buf.size());
        buf.resize(blob_size);

        seq_coll.set_buffer(i, buf);

        k_mer_progress_count.fetch_add(1);

    } // for i
}

static
void generate_k_mers_parallel(CSequenceColl& seq_coll, unsigned k_size,
                              unsigned concurrency)
{
    size_t total_seq_size = seq_coll.total_seq_size();

    if (!concurrency)
        concurrency = 1;

    size_t batch_size = total_seq_size / concurrency;
    if (!batch_size)
        batch_size = total_seq_size;
    std::vector<std::future<void> > futures;

    for (size_t from = 0; from <= seq_coll.size(); )
    {
        size_t to = from;
        for (size_t to_pick = 0; to < seq_coll.size(); ++to)
        {
            to_pick += seq_coll.seq_size(to);
            if (to_pick >= batch_size)
                break;
        } // for

        cout << " [" << from << ".." << to << "] " << (to-from+1) << endl;

        futures.emplace_back(
            std::async(std::launch::async,
            [&seq_coll, k_size, from, to]() { generate_k_mers(seq_coll, k_size, from, to); }
            ));

        from = to+1;
    } // for from

    // wait for all jobs to finish, print progress report
    //
    unsigned long long cnt = seq_coll.size();
    for (auto& e : futures)
    {
        unsigned long long c_prev = 0;
        while(1)
        {
            std::future_status status = e.wait_for(std::chrono::seconds(60));
            if (status == std::future_status::ready)
                break;

            // progress report (entertainment)
            //
            unsigned long long c = k_mer_progress_count;
            auto delta = c - c_prev;
            c_prev = c;

            auto remain_cnt = cnt - c;
            auto remain_min = remain_cnt / delta;
            cout << "\r" << c << ": progress per minute=" << delta;
            if (remain_min < 120)
            {
                 cout << " wait for " << remain_min << "m     " << flush;
            }
            else
            {
                auto remain_h = remain_min / 60;
                cout << " wait for " << remain_h << "h     " << flush;
            }
        } // while
    } // for
    cout << endl;
}

static
void compute_distances(const CSequenceColl& seq_coll,
                       size_t idx,
                       float similarity_cut_off)
{
    assert(similarity_cut_off < 1);
    std::vector<std::pair<unsigned, unsigned> > cand_vect;

    auto sz = seq_coll.buf_size();

    if (idx >= sz)
        return;

    const unsigned char* buf = seq_coll.get_buf(idx);
    if (!buf)
        return;

    bm::bvector<> bv;
    bm::deserialize(bv, buf);

    auto i_cnt = bv.count();

    // approximate number of k-mers we consider similar
    float similarity_target = i_cnt * similarity_cut_off;

    bm::operation_deserializer<bm::bvector<> > od;

    for (size_t i = 0; i < sz; ++i)
    {
        if (i == idx) // self distance (diagonal element)
            continue;

        buf = seq_coll.get_buf(i);
        if (!buf)
            continue;
        // constant deserializer AND just to count the product
        // without actual deserialization (from the compressed BLOB)
        //
        bm::id64_t and_cnt = od.deserialize(bv, buf, 0, bm::set_COUNT_AND);

        if (and_cnt && and_cnt > similarity_target) // similar enough to be a candidate
        {
            cand_vect.push_back(std::make_pair(unsigned(and_cnt), unsigned(i)));
        }

    } // for i

    if (cand_vect.size())
    {
        std::sort(cand_vect.begin(), cand_vect.end());

        for (size_t i = 0; i < cand_vect.size(); ++i)
        {
            const auto p = cand_vect[i];
            float s_factor = float(p.first)/float(i_cnt);
            cout << s_factor << " - " << p.second << endl;
        }

        cout << "idx = " << idx << endl;
        cout << "number of k-mers in the base vector = " << i_cnt << endl;
        cout << "Total cluster size = " << cand_vect.size() << endl << endl;;



    }
    else // no similarities above the cut-off found
    {
        // what to do?
        cout << "Empty set" << endl;
    }

}
/*
static
void compute_distances(distance_matrix_type& dm, const CSequenceColl& seq_coll)
{
    auto sz = seq_coll.buf_size();
    for (size_t i = 0; i < sz; ++i)
    {
        compute_distances(dm, seq_coll, i);
        cout << "\r" << i << flush;
    }
}
*/

int main(int argc, char *argv[])
{
    CSequenceColl          seq_coll;

    try
    {
        auto ret = parse_args(argc, argv);
        if (ret != 0)
        {
            cerr << "cmd-line parse error. " << endl;
            return ret;
        }

        if (!ifa_name.empty()) // FASTA file load
        {
            bm::chrono_taker tt1("1. Load FASTA", 1, &timing_map);

            // limitation: loads a single molecule only
            //
            auto res = load_FASTA(ifa_name, seq_coll);
            if (res != 0)
                return res;
        }

        cout << "Sequences size = " << seq_coll.size() << endl;

        if (ik_size && !ifa_name.empty())
        {
            {
                bm::chrono_taker tt1("2. Generate k-mers", 1, &timing_map);
                seq_coll.sync_buffers_size();
                generate_k_mers_parallel(seq_coll, ik_size, parallel_jobs);
            }

            if (!ikd_name.empty())
            {
                bm::chrono_taker tt1("3. Save k-mers", 1, &timing_map);
                save_kmer_buffers(ikd_name, seq_coll);
            }
        }

        if (ik_size && ifa_name.empty() && !ikd_name.empty())
        {
            {
            bm::chrono_taker tt1("4. Load k-mers", 1, &timing_map);
            load_kmer_buffers(ikd_name, seq_coll);
            }

            if (seq_coll.buf_size())
            {
                bm::chrono_taker tt1("5. compute k-mer similarity", 1, &timing_map);
//                distance_matrix_type dm(seq_coll.buf_size(), seq_coll.buf_size());
//                dm.init();
//                dm.set_zero();


                bm::bvector<> bv_total;
                bv_total.set_range(0, seq_coll.buf_size());
cout << bv_total.count() << endl;;
                bm::random_subset<bm::bvector<> > rsub; // sub-set getter
                bm::bvector<> bv_rsub; // random subset of sequences

                rsub.sample(bv_rsub, bv_total, 10); // pick random sequences
cout << bv_rsub.count() << endl;

                bm::bvector<>::enumerator en(bv_rsub.first());
                for (; en.valid(); ++en)
                {
                    auto idx = *en;
                    cout << idx << endl;
                    compute_distances(seq_coll, idx, float(0.05));
                }
            }
        }


        if (is_timing)
        {
            std::cout << std::endl << "Performance:" << std::endl;
            bm::chrono_taker::print_duration_map(timing_map, bm::chrono_taker::ct_time);
        }

    }
    catch(std::exception& ex)
    {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
    
    

    return 0;
}
