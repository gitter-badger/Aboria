/*

Copyright (c) 2005-2016, University of Oxford.
All rights reserved.

University of Oxford means the Chancellor, Masters and Scholars of the
University of Oxford, having an administrative office at Wellington
Square, Oxford OX1 2JD, UK.

This file is part of Aboria.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
 * Neither the name of the University of Oxford nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

//
// Acknowledgement: This source was modified from the Thrust workshop git repository by Jared Hoberock, https://github.com/jaredhoberock/thrust-workshop
//


#ifndef OCTTREE_H_
#define OCTTREE_H_

#include "CudaInclude.h"
#include "detail/SpatialUtil.h"
#include "Particles.h"

namespace Aboria {

template <typename Traits>
class octtree_query; 

template <typename Traits>
class octtree: 
    public neighbour_search_base<octtree<Traits>,
                                 Traits,
                                 octtree_query<Traits>> {

    typedef typename Traits::double_d double_d;
    typedef typename Traits::bool_d bool_d;
    typedef typename Traits::position position;
    typedef typename Traits::vector_double_d_const_iterator vector_double_d_const_iterator;
    typedef typename Traits::vector_unsigned_int_iterator vector_unsigned_int_iterator;
    typedef typename Traits::vector_unsigned_int vector_unsigned_int;
    typedef typename Traits::vector_int vector_int;
    typedef typename Traits::unsigned_int_d unsigned_int_d;
    typedef typename Traits::template vector_type<int2>::type vector_int2;
    static const unsigned int dimension = Traits::dimension;

    // number of children = 2^d
    static const size_t nchild = (1 << dimension);

    typedef typename Traits::iterator iterator;

    typedef neighbour_search_base<octtree<Traits>,
                                 Traits,
                                 octtree_query<Traits>> base_type;

    friend base_type;


public:
    octtree():base_type() {}
    static constexpr bool cheap_copy_and_delete_at_end() {
        return false;
    }

private:


    void set_domain_impl() {
        const size_t n = this->m_particles_end - this->m_particles_begin;
    }
    void update_iterator_impl() {
        this->m_query.m_particles_begin = iterator_to_raw_pointer(this->m_particles_begin);
    }

    void embed_points_impl() {
        const size_t num_points  = this->m_particles_end - this->m_particles_begin;

        m_tags.resize(num_points);

        /******************************************
         * 3. Classify points                     *
         ******************************************/

        detail::transform(get<position>(this->m_particles_begin), 
                get<position>(this->m_particles_end), 
                m_tags.begin(), 
                classify_point(m_bounds, m_max_level));

                // Now that we have the geometric information, we can sort the
        // points accordingly.
        sort_by_tags();
        
        build_tree();

    }

    void add_points_at_end_impl(const size_t dist) {
        const size_t num_points  = this->m_particles_end - this->m_particles_begin;
        auto start_adding_particles = this->m_particles_end-dist;
        m_tags.resize(num_points);
        auto start_adding_tags = this->m_tags.end()-dist;

        /******************************************
         * 3. Classify new points                 *
         ******************************************/
        detail::transform(get<position>(start_adding_particles), 
                get<position>(this->m_particles_end), 
                start_adding_tags, 
                classify_point(m_bounds, m_max_level));

        // sort and then build tree
        sort_by_tags();
        build_tree();
    }


    void delete_points_at_end_impl(const size_t dist) {
        const size_t n = this->m_particles_end - this->m_particles_begin;

        m_tags.resize(n);
        build_tree();
    }
     
    void copy_points_impl(iterator copy_from_iterator, iterator copy_to_iterator) {
        auto positions_from = get<position>(copy_from_iterator);
        auto positions_to = get<position>(copy_to_iterator);
    }
    const octtree_query<Traits>& get_query_impl() const {
        return this->m_query;
    }

    void sort_by_tags() {
        /******************************************
         * 4. Sort according to classification    *
         ******************************************/
        if (m_tags.size() > 0) {
            m_indices.resize(m_tags.size());
            detail::sequence(m_indices.begin(), m_indices.end());
            detail::sort_by_key(m_tags.begin(), m_tags.end(), m_indices.begin());
            detail::reorder_destructive(m_indices.begin(), 
                                        m_indices.end(), 
                                        this->m_particles_begin);
        }
    }
private:
    void build_tree();

    struct classify_point;
    struct child_index_to_tag_mask;
    struct classify_node;
    struct write_nodes;
    struct make_leaf;

    int max_points;
    int m_max_level;

    vector_int m_tags;
    vector_int m_indices;
    vector_int m_nodes;
    vector_int m_node_tags;
    vector_int2 m_leaves;
    detail::bbox<dimension> m_bounds;

    octtree_query<Traits> m_query;
};



template <typename traits>
void octtree<traits>::build_tree() {
    m_nodes.clear();
    m_node_tags.clear();
    m_leaves.clear();
    vector_int active_nodes(1,0);

    // Build the tree one level at a time, starting at the root
    for (int level = 1 ; !active_nodes.empty() && level <= m_max_level ; ++level)
    {
        /******************************************
         * 1. Calculate children                  *
         ******************************************/

        // New children: 2^D quadrants per active node
        vector_int children(nchild*active_nodes.size());

        // For each active node, generate the tag mask for each of its 2^D children
        detail::tabulate(children.begin(), children.end(),
                child_index_to_tag_mask(level, m_max_level, active_nodes.data()));

        /******************************************
         * 2. Determine interval for each child   *
         ******************************************/

        // For each child we need interval bounds
        vector_int lower_bounds(children.size());
        vector_int upper_bounds(children.size());

        // Locate lower and upper bounds for points in each quadrant
        detail::lower_bound(m_tags.begin(),
                m_tags.end(),
                children.begin(),
                children.end(),
                lower_bounds.begin());

        int length = (1 << (m_max_level - level) * 2) - 1;

        detail::upper_bound(m_tags.begin(),
                m_tags.end(),
                detail::make_transform_iterator(children.begin(), detail::_1 + length),
                detail::make_transform_iterator(children.end(), detail::_1 + length),
                upper_bounds.begin());

        /******************************************
         * 3. Mark each child as empty/leaf/node  *
         ******************************************/

        // Mark each child as either empty, a node, or a leaf
        vector_int child_node_kind(children.size(), 0);
        detail::transform(
                //TODO: boost make_zip_iterator does not work with std::tuple, so
                //have to use the boost make_tuple here
                detail::make_zip_iterator(
                    detail::make_tuple(
                        lower_bounds.begin(), upper_bounds.begin()
                        )),
                detail::make_zip_iterator(
                    detail::make_tuple(
                        lower_bounds.end(),upper_bounds.end()
                        )),
                child_node_kind.begin(),
                classify_node(this->m_n_particles_in_leaf, level == m_max_level));


        /******************************************
         * 4. Enumerate nodes and leaves          *
         ******************************************/

        // Enumerate the nodes and leaves at this level
        vector_int leaves_on_this_level(child_node_kind.size());
        vector_int nodes_on_this_level(child_node_kind.size());

        // Enumerate nodes at this level
        detail::transform_exclusive_scan(child_node_kind.begin(), 
                child_node_kind.end(), 
                nodes_on_this_level.begin(), 
                detail::is_a<detail::NODE>(), 
                0, 
                std::plus<int>());

        // Enumerate leaves at this level
        detail::transform_exclusive_scan(child_node_kind.begin(), 
                child_node_kind.end(), 
                leaves_on_this_level.begin(), 
                detail::is_a<detail::LEAF>(), 
                0, 
                std::plus<int>());

        int num_nodes_on_this_level = nodes_on_this_level.back() + (child_node_kind.back() == detail::NODE ? 1 : 0);
        int num_leaves_on_this_level = leaves_on_this_level.back() + (child_node_kind.back() == detail::LEAF ? 1 : 0);


        /******************************************
         * 5. Add the children to the node list   *
         ******************************************/

        int num_children = child_node_kind.size();

        int children_begin = m_nodes.size();
        m_nodes.resize(m_nodes.size() + num_children);

        detail::transform(
                detail::make_zip_iterator(
                    detail::make_tuple(child_node_kind.begin(), nodes_on_this_level.begin(), leaves_on_this_level.begin())),
                detail::make_zip_iterator(
                    detail::make_tuple(child_node_kind.end(), nodes_on_this_level.end(), leaves_on_this_level.end())),
                m_nodes.begin() + children_begin,
                write_nodes(m_nodes.size(), m_leaves.size()));

        /*******************************************************
         * 5.1. Add the children's tags to the node tag list   *
         *******************************************************/

        m_node_tags.resize(m_nodes.size());

        detail::copy(children.begin(), children.end(),
                m_node_tags.begin() + children_begin);

        /******************************************
         * 6. Add the leaves to the leaf list     *
         ******************************************/

        children_begin = m_leaves.size();

        m_leaves.resize(m_leaves.size() + num_leaves_on_this_level);

        detail::scatter_if(detail::make_transform_iterator(
                    detail::make_zip_iterator(
                        detail::make_tuple(lower_bounds.begin(), upper_bounds.begin())),
                    make_leaf()),
                detail::make_transform_iterator(
                    detail::make_zip_iterator(
                        detail::make_tuple(lower_bounds.end(), upper_bounds.end())),
                    make_leaf()),
                leaves_on_this_level.begin(),
                child_node_kind.begin(),
                m_leaves.begin() + children_begin,
                detail::is_a<detail::LEAF>());


        /******************************************
         * 7. Set the nodes for the next level    *
         ******************************************/

        // Set active nodes for the next level to be all the childs nodes from this level
        active_nodes.resize(num_nodes_on_this_level);

        detail::copy_if(children.begin(),
                children.end(),
                child_node_kind.begin(),
                active_nodes.begin(),
                detail::is_a<detail::NODE>());

    }
}

// Classify a point with respect to the bounding box.
template <typename traits>
struct octtree<traits>::classify_point {
    detail::bbox<dimension> box;
    int max_level;

    // Create the classifier
    classify_point(const detail::bbox<dimension> &b, int lvl) : box(b), max_level(lvl) {}

    // Classify a point
    inline CUDA_HOST_DEVICE
    int operator()(const double_d &p) { return point_to_tag(p, box, max_level); }
};


template <typename traits>
struct octtree<traits>::child_index_to_tag_mask {
    const int level, max_level;

    // mask for lower n bits, where n is the number of dimensions
    const static unsigned mask = nchild - 1;

    typedef typename vector_int::const_pointer ptr_type;
    ptr_type m_nodes;

    child_index_to_tag_mask(int lvl, int max_lvl, ptr_type nodes) : level(lvl), max_level(max_lvl), m_nodes(nodes) {}

    inline CUDA_HOST_DEVICE
    int operator()(int idx) const
    {
        int tag = m_nodes[idx/nchild];
        int which_child = (idx&mask);
        return detail::child_tag_mask(tag, which_child, level, max_level);
    }
};


template <typename traits>
struct octtree<traits>::classify_node
{
    const int threshold;
    const int last_level;

    classify_node(int threshold, int last_level) : threshold(threshold), last_level(last_level) {}

    template <typename tuple_type>
    inline CUDA_HOST_DEVICE
    int operator()(const tuple_type& t) const
    {
        int lower_bound= get<0>(t);
        int upper_bound= get<1>(t);
        const int count = upper_bound - lower_bound;
        if (count == 0)
        {
            return detail::EMPTY;
        }
        else if (last_level || count < threshold)
        {
            return detail::LEAF;
        }
        else
        {
            return detail::NODE;
        }
    }
};

template <typename traits>
struct octtree<traits>::write_nodes {
    int num_nodes, num_leaves;

    write_nodes(int num_nodes, int num_leaves) : 
        num_nodes(num_nodes), num_leaves(num_leaves) 
    {}

    template <typename tuple_type>
    inline CUDA_HOST_DEVICE
    int operator()(const tuple_type &t) const
    {
        int node_type = get<0>(t);
        int node_idx  = get<1>(t);
        int leaf_idx  = get<2>(t);

        if (node_type == detail::EMPTY)
        {
            return detail::get_empty_id();
        }
        else if (node_type == detail::LEAF)
        {
            return detail::get_leaf_id(num_leaves + leaf_idx);
        }
        else
        {
            return num_nodes + nchild * node_idx;
        }
    }
};

template <typename traits>
struct octtree<traits>::make_leaf {
    typedef int2 result_type;
    template <typename tuple_type>
        inline CUDA_HOST_DEVICE
        result_type operator()(const tuple_type &t) const
        {
            int x = get<0>(t);
            int y = get<1>(t);

            return result_type(x, y);
        }
};

template <unsigned int D>
class octtree_child_iterator {
    typedef Vector<double,D> double_d;
    typedef Vector<int,D> int_d;
    typedef Vector<bool,D> bool_d;
    typedef detail::bbox<D> box_type;

    int  m_high;
    int* m_index;
    box_type m_bounds;
public:
    typedef const int* pointer;
	typedef std::forward_iterator_tag iterator_category;
    typedef const int* value_type;
    typedef const int*& reference;
	typedef std::ptrdiff_t difference_type;

    octtree_child_iterator():
        m_index(nullptr)
    {}

    CUDA_HOST_DEVICE
    octtree_child_iterator(int* const start, const box_type& bounds):
        m_high(0),
        m_index(start),
        m_bounds(bounds)
    {}

    bool is_high(const size_t i) {
        return m_high & (1<<i);
    }

    box_type& get_bounds() {
        return m_bounds;
    }

    CUDA_HOST_DEVICE
    reference operator *() const {
        return dereference();
    }

    CUDA_HOST_DEVICE
    reference operator ->() const {
        return dereference();
    }

    CUDA_HOST_DEVICE
    octtree_child_iterator& operator++() {
        increment();
        return *this;
    }

    CUDA_HOST_DEVICE
    inline bool operator==(const octtree_child_iterator& rhs) const {
        return equal(rhs);
    }

    CUDA_HOST_DEVICE
    inline bool operator!=(const octtree_child_iterator& rhs) const {
        return !operator==(rhs);
    }

    inline bool operator==(const bool rhs) const {
        return equal(rhs);
    }

    inline bool operator!=(const bool rhs) const {
        return !operator==(rhs);
    }

private:

    CUDA_HOST_DEVICE
    bool equal(octtree_child_iterator const& other) const {
        return (m_index == other.m_index).all();
    }

    bool equal(const bool other) const {
        return (m_high<(1<<D))==other;
    }

    CUDA_HOST_DEVICE
    reference dereference() const { 
        return m_index; 
    }

    CUDA_HOST_DEVICE
    void increment() {
        ++m_index;
        ++m_high;
        for (int i = 0; i < D; ++i) {
            const bool high = m_high & (1<<i);
            m_bounds.bmin[i] = high?
                                0.5*(m_bounds.bmax[i]+m_bounds.bmin[i]):
                                m_bounds.bmin[i];
        }
    }
};


template <unsigned int D>
class octtree_child_query_iterator {
    typedef Vector<double,D> double_d;
    typedef Vector<int,D> int_d;
    typedef Vector<bool,D> bool_d;
    typedef detail::bbox<D> box_type;
    typedef octtree_child_iterator<D> child_iterator;
    static const unsigned int nchild = (1<<D);

    std::array<int,nchild> m_indices;
public:
    typedef const int* pointer;
	typedef std::forward_iterator_tag iterator_category;
    typedef const int* value_type;
    typedef const int*& reference;
	typedef std::ptrdiff_t difference_type;

    octtree_child_query_iterator():
        m_index(nullptr)
    {}

    CUDA_HOST_DEVICE
    octtree_child_query_iterator(const child_iterator& node, const double_d query_point, const double_d cut_dist):
        m_high(0),
        m_index(start),
        m_bounds(bounds)
    {
            std::array<float,nchild> distances;

            // loop through all children
            for (int i = 0; i < nchild; ++i) {
                
                double_d shortest_dist;
                for (int j = 0; j < dimension; j++) {
                    // zero dist if query_point is in this child, keep original
                    // dist (neg or pos) if not
                    //
                    const bool is_high = i & (1<<i);
                    shortest_dist[j] = (is_high^(dist[j]>0))*dist[j]; 
                }
                                    
                // calculate norm of m_dists 
                double accum = 0;
                for (int i = 0; i < dimension; ++i) {
                    accum = detail::distance_helper<LNormNumber>::accumulate_norm(accum,dist[i]*m_inv_max_distance[i]); 
                }
                if (accum < 1.0) { // child possible
                    if (accum == 0.0) { // this is the best child
                        m_node = m_query->get_child(m_node,i);
                    } else { // save possible child to stack for later
                        m_stack.push(m_query->get_child(m_node,i));
                    }
                }
            }
            // m_node should now be updated with the best child, moving on...
        }

    
    
    }

    bool is_high(const size_t i) {
        return m_high & (1<<i);
    }

    box_type& get_bounds() {
        return m_bounds;
    }

    CUDA_HOST_DEVICE
    reference operator *() const {
        return dereference();
    }

    CUDA_HOST_DEVICE
    reference operator ->() const {
        return dereference();
    }

    CUDA_HOST_DEVICE
    octtree_child_iterator& operator++() {
        increment();
        return *this;
    }

    CUDA_HOST_DEVICE
    inline bool operator==(const octtree_child_iterator& rhs) const {
        return equal(rhs);
    }

    CUDA_HOST_DEVICE
    inline bool operator!=(const octtree_child_iterator& rhs) const {
        return !operator==(rhs);
    }

    inline bool operator==(const bool rhs) const {
        return equal(rhs);
    }

    inline bool operator!=(const bool rhs) const {
        return !operator==(rhs);
    }

private:

    CUDA_HOST_DEVICE
    bool equal(octtree_child_iterator const& other) const {
        return (m_index == other.m_index).all();
    }

    bool equal(const bool other) const {
        return (m_high<(1<<D))==other;
    }

    CUDA_HOST_DEVICE
    reference dereference() const { 
        return m_index; 
    }

    CUDA_HOST_DEVICE
    void increment() {
        ++m_index;
        ++m_high;
        for (int i = 0; i < D; ++i) {
            const bool high = m_high & (1<<i);
            m_bounds.bmin[i] = high?
                                0.5*(m_bounds.bmax[i]+m_bounds.bmin[i]):
                                m_bounds.bmin[i];
        }
    }
};


template <typename Query>
class octtree_depth_first_iterator {
    typedef tree_depth_first_iterator<Query> iterator;
    typedef typename Query::child_iterator child_iterator;
    static const unsigned int dimension = Query::dimension;
    typedef Vector<double,dimension> double_d;
    typedef Vector<int,dimension> int_d;

public:
    typedef typename child_iterator::value_type value_type;
    typedef child_iterator pointer;
	typedef std::forward_iterator_tag iterator_category;
    typedef typename child_iterator::reference reference;
	typedef std::ptrdiff_t difference_type;

    CUDA_HOST_DEVICE
    octtree_depth_first_iterator()
    {}
       
    /// this constructor is used to start the iterator at the head of a bucket 
    /// list
    CUDA_HOST_DEVICE
    octtree_depth_first_iterator(const child_iterator& start_node,
                        const Query *query
                  ):
        m_query(query)
    {
        m_stack.push(start_node); 
    }

    CUDA_HOST_DEVICE
    reference operator *() const {
        return dereference();
    }
    CUDA_HOST_DEVICE
    reference operator ->() {
        return dereference();
    }
    CUDA_HOST_DEVICE
    iterator& operator++() {
        increment();
        return *this;
    }
    CUDA_HOST_DEVICE
    iterator operator++(int) {
        iterator tmp(*this);
        operator++();
        return tmp;
    }
    CUDA_HOST_DEVICE
    size_t operator-(iterator start) const {
        size_t count = 0;
        while (start != *this) {
            start++;
            count++;
        }
        return count;
    }
    CUDA_HOST_DEVICE
    inline bool operator==(const iterator& rhs) const {
        return equal(rhs);
    }
    CUDA_HOST_DEVICE
    inline bool operator!=(const iterator& rhs) const {
        return !operator==(rhs);
    }

 private:
    friend class boost::iterator_core_access;

    CUDA_HOST_DEVICE
    void increment() {
#ifndef __CUDA_ARCH__
        LOG(4,"\tincrement (octtree_depth_first_iterator):"); 
#endif
        if (m_query->is_leaf_node(m_stack.top())) {
            ++m_stack.top();
            if (m_stack.top() == false) {
                m_stack.pop();
            }
        } else {
            m_stack.push(m_query->get_child(m_stack.top()));
        }

#ifndef __CUDA_ARCH__
        LOG(4,"\tend increment (octtree_depth_first_iterator): m_node = "<<m_node); 
#endif
    }

    CUDA_HOST_DEVICE
    bool equal(iterator const& other) const {
        return m_stack.top() == other.m_stack.top();
    }

    CUDA_HOST_DEVICE
    reference dereference() const
    { return *m_stack.top(); }


    std::stack<child_iterator> m_stack;
    const Query *m_query;
};

template <typename Query, int LNormNumber>
class octtree_query_iterator {
    typedef octtree_query_iterator<Query,LNormNumber> iterator;
    typedef typename Query::child_iterator child_iterator;
    static const unsigned int dimension = Query::dimension;
    typedef Vector<double,dimension> double_d;
    typedef Vector<int,dimension> int_d;

public:
    typedef typename child_iterator::value_type value_type;
    typedef child_iterator pointer;
	typedef std::forward_iterator_tag iterator_category;
    typedef typename child_iterator::reference reference;
	typedef std::ptrdiff_t difference_type;

    CUDA_HOST_DEVICE
    octtree_query_iterator()
    {}
       
    /// this constructor is used to start the iterator at the head of a bucket 
    /// list
    CUDA_HOST_DEVICE
    octtree_query_iterator(const child_iterator& start_node,
                  const double_d& query_point,
                  const double_d& max_distance,
                  const Query *query
                  ):
        m_query_point(query_point),
        m_inv_max_distance(1.0/max_distance),
        m_query(query)
    {
        if (start_node == nullptr) {
                LOG(4,"\tocttree_query_iterator (constructor) empty tree, returning default iterator");
        } else {
            double accum = 0;
            const box_type& bounds = m_query->get_bounds(start_node);
            for (int i = 0; i < dimension; ++i) {
                double dist = m_query_point[i];
                if (dist < bounds.bmin[i]) {
                    dist -= bounds.bmin[i];
                } else if (dist > bounds.bmax[i]) {
                    dist -= bounds.bmax[i];
                }
                accum = detail::distance_helper<LNormNumber>::accumulate_norm(accum,dist*m_inv_max_distance[i]); 
            }
            if (accum <= 1.0) {
                LOG(4,"\tocttree_query_iterator (constructor) with query pt = "<<m_query_point<<"): searching root node");
                m_stack.push(m_query->get_child_query(start_node,query_point,max_distance));
                go_to_next_leaf();
            } else {
                LOG(4,"\tocttree_query_iterator (constructor) with query pt = "<<m_query_point<<"): search region outside domain");
            }
        }
    }

    octtree_query_iterator(const iterator& copy):
        m_query_point(copy.m_query_point),
        m_inv_max_distance(copy.m_inv_max_distance)
    {
        m_stack = copy.m_stack;
        //std::copy(copy.m_stack.begin(),copy.m_stack.end(),m_stack.begin()); 
    }

    iterator& operator=(const iterator& copy) {
        m_query_point = copy.m_query_point;
        m_inv_max_distance = copy.m_inv_max_distance;
        m_stack = copy.m_stack;
        return *this;
        //std::copy(copy.m_stack.begin(),copy.m_stack.end(),m_stack.begin()); 
    }

    iterator& operator=(const octtree_depth_first_iterator<Query>& copy) {
#ifndef NDEBUG
        const double_d low = copy.m_query->get_bounds_low(*m_node);
        const double_d high = copy.m_query->get_bounds_high(*m_node);
        ASSERT((low <= m_query_point).all() && (high > m_query_point).all(),"query point not in depth_first_iterator")
#endif
        m_stack = copy.m_stack;
        return *this;
    }


    CUDA_HOST_DEVICE
    reference operator *() const {
        return dereference();
    }
    CUDA_HOST_DEVICE
    reference operator ->() {
        return dereference();
    }
    CUDA_HOST_DEVICE
    iterator& operator++() {
        increment();
        return *this;
    }
    CUDA_HOST_DEVICE
    iterator operator++(int) {
        iterator tmp(*this);
        operator++();
        return tmp;
    }
    CUDA_HOST_DEVICE
    size_t operator-(iterator start) const {
        size_t count = 0;
        while (start != *this) {
            start++;
            count++;
        }
        return count;
    }
    CUDA_HOST_DEVICE
    inline bool operator==(const iterator& rhs) const {
        return equal(rhs);
    }
    CUDA_HOST_DEVICE
    inline bool operator!=(const iterator& rhs) const {
        return !operator==(rhs);
    }

 private:
    friend class boost::iterator_core_access;

    void increment() {

#ifndef __CUDA_ARCH__
        LOG(4,"\tincrement (octtree_query_iterator):"); 
#endif

        ++stack.top();

        while (!stack.empty() && !m_query->is_leaf_node(stack.top())) {
            child_iterator& node = stack.top();
            const bbox& bounds = m_query->get_bounds(node);
            double accum = 0;
            for (int j = 0; j < dimension; j++) {
                const bool less_than_bmin = m_query_point[j]<bounds.bmin[j];
                const bool more_than_bmax = m_query_point[j]>bounds.bmax[j];

                // dist 0 if between min/max, or distance to min/max if not
                const double dist = (less_than_bmin^more_than_bmax)
                                    *(less_than_bmin?
                                            (bounds.bmin[j]-m_query_point[j]):
                                            (m_query_point[j]-bounds.bmax[j]));

                accum = detail::distance_helper<LNormNumber>::
                            accumulate_norm(accum,dist*m_inv_max_distance[j]); 
            }
            if (accum < 1.0) { // could be in this child, so dive down
                stack.push(m_query->get_children(node));
            } else { // not in this one, so go to next child, or go up if no more children
                ++stack.top();
                while (!stack.empty() && stack.top() == false) {
                    stack.pop();
                }
            }
        }
        if (stack.empty()) {
#ifndef __CUDA_ARCH__
            LOG(4,"\tend increment (octree_query_iterator): no more nodes"); 
#endif
        } else {
#ifndef __CUDA_ARCH__
            LOG(4,"\tend increment (octree_query_iterator): m_node = "<<stack.top()); 
#endif
        }
    }
    

    CUDA_HOST_DEVICE
    bool equal(iterator const& other) const {
        return m_node == other.m_node;
    }


    CUDA_HOST_DEVICE
    reference dereference() const
    { return *m_node; }


    std::stack<child_iterator> m_stack;
    double_d m_query_point;
    double_d m_inv_max_distance;
    const Query *m_query;
};



template <typename Traits>
struct octtree_query {
    const static unsigned int dimension = Traits::dimension;

    typedef Traits traits_type;
    typedef typename Traits::raw_pointer raw_pointer;
    typedef typename Traits::double_d double_d;
    typedef typename Traits::bool_d bool_d;
    typedef typename Traits::int_d int_d;
    typedef typename Traits::unsigned_int_d unsigned_int_d;
    typedef octtree_query_iterator<octtree_query,-1> query_iterator;
    typedef octtree_depth_first_iterator<octtree_query> root_iterator;
    typedef octtree_depth_first_iterator<octtree_query> all_iterator;
    typedef octtree_child_iterator<dimension> child_iterator;
    typedef typename query_iterator::reference reference;
    typedef typename query_iterator::pointer pointer;
    typedef typename query_iterator::value_type value_type;
    typedef detail::bbox<dimension> box_type;

    typedef ranges_iterator<Traits> particle_iterator;

    bool_d m_periodic;
    box_type m_bounds;
    raw_pointer m_particles_begin;
    size_t m_number_of_nodes;

    int2* m_leaves_begin;
    int* m_nodes_begin;
    int* m_node_tags_begin;

    const box_type& get_bounds() const { return m_bounds; }
    const bool_d& get_periodic() const { return m_periodic; }

    /*
     * functions for octtree_query_iterator
     */
    pointer get_child(pointer bucket, const int i) {
        return m_nodes_begin + *bucket + i;
    }

    double_d get_bucket_cut(reference bucket) {
        return double_d(0.5);
    }

    /*
     * end functions for octtree_query_iterator
     */

    static bool is_leaf_node(reference bucket) {
        return bucket < 0;
    }

    child_iterator get_children(pointer bucket) {
        CHECK(bucket == m_nodes_begin, "bucket should be a root bucket");
        return child_iterator(m_nodes_begin + *bucket, m_bounds);
    }
    child_iterator get_children(const child_iterator& ci) {
        return child_iterator(&(*ci), ci.get_bounds());
    }

    const box_type& get_bucket_bounds(const child_iterator& ci) {
        return ci.get_bounds();
    }


    iterator_range<particle_iterator> 
    get_bucket_particles(reference bucket) const {
        const int leaf_idx = -m_nodes_begin[bucket];
        ASSERT(leaf_idx > 0, "ERROR: bucket is not a leaf!");
        const int2& particle_idxs = m_leaves_begin[leaf_idx];
#ifndef __CUDA_ARCH__
        LOG(4,"\tget_bucket_particles: looking in bucket with start index = "<<particle_idxs[0]<<" end index = "<<particle_idxs[1]);
#endif        
        return iterator_range<particle_iterator>(
                        particle_iterator(m_particles_begin + particle_idxs[0]),
                        particle_iterator(m_particles_begin + particle_idxs[1]));
    }

    box_type get_child_bounds(const child_iterator<dimension>& ci, reference parent, const box_type& parent_box) {
        box_type ret;
        for (int i = 0; i < dimension; ++i) {
            if (ci.is_high(i)) {
                ret.bmin[i] = parent_box.bmax[i]/2; 
                ret.bmax[i] = parent_box.bmax[i]; 
            } else {
                ret.bmin[i] = parent_box.bmin[i]; 
                ret.bmax[i] = parent_box.bmax[i]/2; 
            }

        }
        return ret;
    }


        const size_t index = &bucket - m_nodes_begin;
        return detail::tag_to_lower_bound(m_node_tags_begin[index]);
    }

    CUDA_HOST_DEVICE
    box_type get_root_bucket_bounds(reference bucket) {
        return get_bounds();
    }


    CUDA_HOST_DEVICE
    reference get_bucket(const double_d &position) const {
        query_iterator it(m_nodes_begin,position,double_d(1),this);
        return *it;
    }
    
    CUDA_HOST_DEVICE
    size_t get_bucket_index(reference bucket) const {
        return &bucket - m_nodes_begin;
    }

    size_t number_of_buckets() const {
        return m_number_of_nodes;
    }

    template <int LNormNumber=-1>
    iterator_range<query_iterator> 
    get_buckets_near_point(const double_d &position, const double max_distance) const {
#ifndef __CUDA_ARCH__
        LOG(4,"\tget_buckets_near_point: position = "<<position<<" max_distance= "<<max_distance);
#endif
        return iterator_range<query_iterator>(
                query_iterator(m_nodes_begin,position,double_d(max_distance),this),
                query_iterator()
                );
    }

    template <int LNormNumber=-1>
    iterator_range<query_iterator> 
    get_buckets_near_point(const double_d &position, const double_d &max_distance) const {
#ifndef __CUDA_ARCH__
        LOG(4,"\tget_buckets_near_point: position = "<<position<<" max_distance= "<<max_distance);
#endif
        return iterator_range<query_iterator>(
                query_iterator(m_nodes_begin,position,max_distance,this),
                query_iterator()
                );
    }

    iterator_range<root_iterator> get_root_buckets() const {
        return iterator_range<root_iterator>(m_nodes_begin, m_nodes_begin+1);
    }

    iterator_range<all_iterator> get_subtree(reference bucket) const {
        return iterator_range<all_iterator>(all_iterator(&bucket,this),all_iterator());
    }

    raw_pointer get_particles_begin() const {
        return m_particles_begin;
    }


};


}
#endif /* OCTTREE_H_ */
