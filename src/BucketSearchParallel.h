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
// Acknowledgement: This source was modified from the Thrust example bucket_sort2d.cu
//


#ifndef BUCKETSEARCH_H_
#define BUCKETSEARCH_H_

#include "Traits.h"
#include "CudaInclude.h"
#include "Vector.h"
#include "SpatialUtil.h"
#include "Get.h"
#include "Log.h"

namespace Aboria {

template <typename traits>
class BucketSearch {
    UNPACK_TRAITS(traits)

public:

    /// A const iterator to a set of neighbouring points. This iterator implements
    /// a STL forward iterator type
	class const_iterator;

    BucketSearch() {
        const double min = std::numeric_limits<double>::min();
        const double max = std::numeric_limits<double>::max();
        set_domain(double_d(min/3.0),double_d(max/3.0),bool_d(false),double_d(max/3.0-min/3.0)); 
    };

    void embed_points(particles_iterator begin, particles_iterator end) {

        m_particles_begin = begin;
        m_particles_end = end;
        m_positions_begin = get<position>(m_particles_begin);
        m_positions_end = get<position>(m_particles_end);

        CHECK(!m_bounds.is_empty(), "trying to embed particles into an empty domain. use the function `set_domain` to setup the spatial domain first.");

        const size_t n = m_particles_end - m_particles_begin;
	    LOG(2,"BucketSearch: embed_points: embedding "<<n<<" points");
        m_bucket_indices.resize(n);

        if (n > 0) {
            build_bucket_indices(m_positions_begin,m_positions_end,m_bucket_indices.begin());
            sort_by_bucket_index();
        }
        build_buckets();
    }


    void add_points_at_end(const particles_iterator &begin, const particles_iterator &start_adding, const particles_iterator &end);

    /// return a const forward iterator to all the points in the neighbourhood of \p r. If 
    /// this function is being used to find all the point pairs within the same point container, then
    /// a naive looping through and using find_broadphase_neighbours() will find each pair twice. 
    /// This can be avoided by setting self=true and supplying the index of each point with my_index
    CUDA_HOST_DEVICE 
    const_iterator find_broadphase_neighbours(const double_d& r, 
                                              const int my_index, 
                                              const bool self) const;

    CUDA_HOST_DEVICE 
    const_iterator end() const { return const_iterator(this); }


    void set_domain(const double_d &min_in, const double_d &max_in, const bool_d& periodic_in, const double_d& side_length) {
        LOG(2,"BucketSearch: set_domain:");
        m_bounds.bmin = min_in;
        m_bounds.bmax = max_in;
        m_periodic = periodic_in;
        m_bucket_side_length = side_length;
        m_size = floor((m_bounds.bmax-m_bounds.bmin)/m_bucket_side_length).template cast<unsigned int>();
        m_bucket_side_length = (m_bounds.bmax-m_bounds.bmin)/m_size;
        m_point_to_bucket_index = point_to_bucket_index<dimension>(m_size,m_bucket_side_length,m_bounds);
 
	    LOG(2,"\tbounds = "<<m_bounds);
	    LOG(2,"\tperiodic = "<<m_periodic);
	    LOG(2,"\tbucket_side_length = "<<m_bucket_side_length);
	    LOG(2,"\tnumber of buckets = "<<m_size<<" (total="<<m_size.prod()<<")");

        // setup bucket data structures
        m_bucket_begin.resize(m_size.prod());
        m_bucket_end.resize(m_size.prod());
    }


    const double_d& get_min() const { return m_bounds.bmin; }
    const double_d& get_max() const { return m_bounds.bmax; }
    const double_d& get_side_length() const { return m_bucket_side_length; }
    const bool_d& get_periodic() const { return m_periodic; }


private:
    void build_buckets();
    void build_bucket_indices(
        vector_double_d_const_iterator positions_begin,
        vector_double_d_const_iterator positions_end,
        vector_unsigned_int_iterator bucket_indices_begin);
    void sort_by_bucket_index();
 


    particles_iterator m_particles_begin;
    particles_iterator m_particles_end;
    vector_double_d_const_iterator m_positions_begin;
    vector_double_d_const_iterator m_positions_end;
    bool_d m_periodic;
    double_d m_bucket_side_length; 
    unsigned_int_d m_size;
    bbox<dimension> m_bounds;
    point_to_bucket_index<dimension> m_point_to_bucket_index;

    // the grid data structure keeps a range per grid bucket:
    // each bucket_begin[i] indexes the first element of bucket i's list of points
    // each bucket_end[i] indexes one past the last element of bucket i's list of points
    vector_unsigned_int m_bucket_begin;
    vector_unsigned_int m_bucket_end;
    vector_unsigned_int m_bucket_indices;

    unsigned_int_d m_surrounding_buckets_offsets;
};


template <typename traits>
void BucketSearch<traits>::build_bucket_indices(
        vector_double_d_const_iterator positions_begin,
        vector_double_d_const_iterator positions_end,
        vector_unsigned_int_iterator bucket_indices_begin
        ) {
    // transform the points to their bucket indices
    transform(positions_begin,
            positions_end,
            bucket_indices_begin,
            m_point_to_bucket_index);
}

template <typename traits>
void BucketSearch<traits>::sort_by_bucket_index() {
    // sort the points by their bucket index
    traits::sort_by_key(m_bucket_indices.begin(),
            m_bucket_indices.end(),
            m_particles_begin);
}


template <typename traits>
void BucketSearch<traits>::build_buckets() {

    // find the beginning of each bucket's list of points
    typename traits::template counting_iterator<unsigned int> search_begin(0);
    traits::lower_bound(m_bucket_indices.begin(),
            m_bucket_indices.end(),
            search_begin,
            search_begin + m_size.prod(),
            m_bucket_begin.begin());

    // find the end of each bucket's list of points
    traits::upper_bound(m_bucket_indices.begin(),
            m_bucket_indices.end(),
            search_begin,
            search_begin + m_size.prod(),
            m_bucket_end.begin());
}


template <typename traits>
void BucketSearch<traits>::add_points_at_end(const particles_iterator &begin, const particles_iterator &start_adding, const particles_iterator &end) {
    m_particles_begin = begin;
    m_particles_end = end;
    m_positions_begin = get<position>(m_particles_begin);
    m_positions_end = get<position>(m_particles_end);

    CHECK(start_adding-begin == m_bucket_indices.size(), "prior number of particles embedded into domain is not consistent with distance between begin and start_adding");
    CHECK(!m_bounds.is_empty(), "trying to embed particles into an empty domain. use the function `set_domain` to setup the spatial domain first.");

    const size_t dist = end - start_adding;
    if (dist > 0) {
        const size_t total = m_bucket_indices.size() + dist;
	    LOG(2,"BucketSearch: add_points_at_end: embedding "<<dist<<" new points. Total number = "<<total);
        vector_double_d_const_iterator positions_start_adding = m_positions_end - dist;

        m_bucket_indices.resize(total);
        vector_unsigned_int_iterator bucket_indices_start_adding = m_bucket_indices.end() - dist;

        build_bucket_indices(positions_start_adding,m_positions_end,bucket_indices_start_adding);
        sort_by_bucket_index();
        build_buckets();
        /*
        for (int i = 0; i<total; ++i) {
            std::cout << "p = "<<*(m_positions_begin+i)<<" index = "<<m_bucket_indices[i]<<std::endl;
        }
        */

    }
}


template <typename traits>
CUDA_HOST_DEVICE
typename BucketSearch<traits>::const_iterator
BucketSearch<traits>::find_broadphase_neighbours(
        const double_d& r, 
        const int my_index, 
        const bool self) const {
    
    ASSERT((r >= m_bounds.bmin).all() && (r < m_bounds.bmax).all(), "Error, search position "<<r<<" is outside neighbourhood search bounds " << m_bounds);
    const unsigned_int_d my_bucket = m_point_to_bucket_index.find_bucket_index_vector(r);

#ifndef __CUDA_ARCH__
	LOG(3,"BucketSearch: find_broadphase_neighbours: around r = "<<r<<". my_index = "<<my_index<<" self = "<<self);
#endif

    const_iterator search_iterator(this,r);
    int_d bucket_offset(-1);
    constexpr unsigned int last_d = dimension-1;
    bool still_going = true;
    while (still_going) {
        unsigned_int_d other_bucket = my_bucket + bucket_offset; 

        // handle end cases
        double_d transpose(0);
        bool outside = false;
        for (int i=0; i<dimension; i++) {
#ifdef __CUDA_ARCH__
            if (other_bucket[i] >= NPP_MAX_16U) {
#else
            if (other_bucket[i] == std::numeric_limits<unsigned int>::max()) {
#endif
                if (m_periodic[i]) {
                    other_bucket[i] = m_size[i]-1;
                    transpose[i] = -(m_bounds.bmax-m_bounds.bmin)[i];
                } else {
                    outside = true;
                    break;
                }
            }
            if (other_bucket[i] == m_size[i]) {
                if (m_periodic[i]) {
                    other_bucket[i] = 0;
                    transpose[i] = (m_bounds.bmax-m_bounds.bmin)[i];
                } else {
                    outside = true;
                    break;
                }
            }
        }

        if (!outside) {
            const unsigned int other_bucket_index = m_point_to_bucket_index.collapse_index_vector(other_bucket);
            const unsigned int range_start_index = m_bucket_begin[other_bucket_index]; 
            const unsigned int range_end_index = m_bucket_end[other_bucket_index]; 

            if (range_end_index-range_start_index > 0) {

                //std::cout << "adding range for my_bucket = "<<my_bucket<<" other_bucket = "<<other_bucket<<" range_start_index = "<<range_start_index<<" range_end_index = "<<range_end_index<< " transpose = "<<transpose<<std::endl;
                search_iterator.add_range(
                        m_particles_begin + range_start_index,
                        m_particles_begin + range_end_index,
                        transpose);
            }

        }

        // go to next candidate bucket
        for (int i=0; i<dimension; i++) {
            bucket_offset[i]++;
            if (bucket_offset[i] <= 1) break;
            if (i == last_d) still_going = false;
            bucket_offset[i] = -1;
        }
    }
    
    return search_iterator;
}



template <typename traits>
class BucketSearch<traits>::const_iterator {
public:
    typedef const std::tuple<const particles_value_type&,const double_d&>* pointer;
	typedef std::forward_iterator_tag iterator_category;
    typedef const std::tuple<particles_reference_type,const double_d&> reference;
    typedef const std::tuple<particles_reference_type,const double_d&> value_type;
	typedef std::ptrdiff_t difference_type;
    
    CUDA_HOST_DEVICE
    const_iterator(const BucketSearch<traits>* bucket_sort):
        m_bucket_sort(bucket_sort),
        m_node(bucket_sort->m_particles_end) {}

    CUDA_HOST_DEVICE
    const_iterator(const BucketSearch<traits>* bucket_sort, const double_d &r):
        m_bucket_sort(bucket_sort),
        m_r(r),
        m_node(bucket_sort->m_particles_end),
        m_begins(),
        m_ends(),
        m_transpose() 
    {}

    CUDA_HOST_DEVICE
    void add_range(particles_iterator begin, particles_iterator end, const double_d &transpose) {
        m_begins.push_back(begin);
        m_ends.push_back(end);
        m_transpose.push_back(transpose);
        if (m_node == m_bucket_sort->m_particles_end) {
            m_current_index = m_begins.size()-1;
            m_node = m_begins[m_current_index];
            if (!check_candidate()) {
                increment(); 
            }
        }
    }

    CUDA_HOST_DEVICE
    bool equal(const_iterator const& other) const {
        return m_node == other.m_node;
    }

    CUDA_HOST_DEVICE
    reference dereference() const { 
        return reference(*m_node,m_dx); 
    }

    CUDA_HOST_DEVICE
    bool go_to_next_candidate() {
        m_node++;
        if (m_node == m_ends[m_current_index]) {
            m_current_index++;
            //std::cout << "moving on to next index i = "<<m_current_index<<" with range "<<m_begins[m_current_index]-m_bucket_sort->m_particles_begin<<" to "<<m_ends[m_current_index]-m_bucket_sort->m_particles_begin<<std::endl;
            if (m_current_index < m_begins.size()) {
                m_node = m_begins[m_current_index];
                //std::cout << "particle index = "<<m_node-m_bucket_sort->m_particles_begin<<std::endl;
            } else {
                m_node = m_bucket_sort->m_particles_end;
                return false;
            }
        }
        return true;
    }

    CUDA_HOST_DEVICE
    bool check_candidate() {
        //std::cout << "check my_r = "<<m_r<<" r = "<<get<position>(*m_node)<<" trans = "<<m_transpose[m_current_index]<<" index = "<<m_current_index<<std::endl;
        const double_d p = get<position>(*m_node) + m_transpose[m_current_index];
        m_dx = p - m_r;

        bool outside = false;
        for (int i=0; i < dimension; i++) {
            if (std::abs(m_dx[i]) > m_bucket_sort->m_bucket_side_length[i]) {
                outside = true;
                break;
            } 
        }

        return !outside;
    }

    CUDA_HOST_DEVICE
    void increment() {
        bool found_good_candidate = false;
        while (!found_good_candidate && go_to_next_candidate()) {
            found_good_candidate = check_candidate();
        }
    }

    CUDA_HOST_DEVICE
    reference operator *() {
        return dereference();
    }

    CUDA_HOST_DEVICE
    reference operator ->() {
        return dereference();
    }

    CUDA_HOST_DEVICE
    const_iterator& operator++() {
        increment();
        return *this;
    }

    CUDA_HOST_DEVICE
    const_iterator operator++(int) {
        const_iterator tmp(*this);
        operator++();
        return tmp;
    }

    CUDA_HOST_DEVICE
    size_t operator-(const_iterator start) const {
        size_t count = 0;
        while (start != *this) {
            start++;
            count++;
        }
        return count;
    }

    CUDA_HOST_DEVICE
    inline bool operator==(const const_iterator& rhs) {
        return equal(rhs);
    }

    CUDA_HOST_DEVICE
    inline bool operator!=(const const_iterator& rhs){
        return !operator==(rhs);
    }

private:
    friend class boost::iterator_core_access;

    
    const BucketSearch* m_bucket_sort;
    double_d m_r;
    double_d m_dx;
    particles_iterator m_node;
#ifdef HAVE_THRUST
#ifdef __CUDA_ARCH__
    std::vector<particles_iterator> m_begins;
    thrust::device_vector<particles_iterator> m_ends;
    thrust::device_vector<double_d> m_transpose;
#else
    std::vector<particles_iterator> m_begins;
    thrust::host_vector<particles_iterator> m_ends;
    thrust::host_vector<double_d> m_transpose;
#endif
#else
    std::vector<particles_iterator> m_begins;
    std::vector<particles_iterator> m_ends;
    std::vector<double_d> m_transpose;
#endif
    int m_current_index = -1;
};

}


#endif /* BUCKETSEARCH_H_ */