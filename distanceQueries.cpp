// ======================================================================== //
// Copyright 2009-2017 Ingo Wald                                            //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "distanceQueries.h"
#include "bvh.h"

#include <vector>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <queue>

namespace bvhlib {

  /*! result of a single closest-point query */
  struct QueryResult {
    
    /*! closest point on given triangle */
    vec3f   point;
    
    /*! distance to query point */
    float   distance;
    
    /* primitmive it in scene object */
    int32_t primID;
  };
  
  inline float clamp01(const float a, float b)
  {
    if (fabsf(b) < 1e-12f) b = 1e-12f;
    float f = a / b;
    return std::min(std::max(f,0.f),1.f);
  }

  /*! compute the closest point P' to P on triangle ABC, and return it */
  inline vec3f closestPoint(const vec3f &A,
                            const vec3f &B,
                            const vec3f &C,
                            const vec3f &P)
  {
    const vec3f e0 = B - A;
    const vec3f e1 = C - A;
    const vec3f v0 = A - P;
      
    const float a = dot(e0,e0);
    const float b = dot(e0,e1);
    const float c = dot(e1,e1);
    const float d = dot(e0,v0);
    const float e = dot(e1,v0);

    float det = a*c-b*b; 
    float s   = b*e - c*d;
    float t   = b*d - a*a;

    if (s+t < det) {
      if (s < 0.f) {
        if (t < 0.f) {
          if (d < 0.d) {
            s = clamp01(-d,a);
            t = 0.f;
          } else {
            s = 0.f;
            t = clamp01(-e,c);
          }
        } else {
          s = 0.f;
          t = clamp01(-e,c);
        }
      } else if (t < 0.f) {
        s = clamp01(-d,a);
        t = 0.f;
      } else {
        const float invDet = 1.f/det;
        s *= invDet;
        t *= invDet;
      }
    } else {
      if (s < 0.f) {
        float tmp0 = b+d;
        float tmp1 = c+e;
        if (tmp1 > tmp0) {
          float num = tmp1-tmp0;
          float den = a-2.f*b+c;
          s = clamp01(num,den);
          t = 1.f - s;
        } else {
          s = 0.f;
          t = clamp01(-e,c);
        }
      } else if (t < 0.f) {
        if (a+d > b+e) {
          float num= c+e-b-d;
          float den = a-2.f*b+c;
          s = clamp01(num,den);
          t = 1.f-s;
        } else {
          s = clamp01(-e,c);
          t = 0.f;
        }
      } else {
        float num = c+e-b-d;
        float den = a-2.f*b+c;
        s = clamp01(num,den);
        t = 1.f - s;
      }
    }
    return A + s*e0 + t*e1;
  }
    
  struct QueryObject : public bvhlib::Geometry {
    virtual ~QueryObject() {};
    // virtual void computeBounds(RTCBounds &bounds, const size_t primID) = 0;
    virtual void updateIfCloser(QueryResult &resut,
                                const vec3f &P,
                                const size_t primID) = 0;

    BVH       bvh;
  };

  template<typename coord_t, typename index_t>
  struct GeneralTriangleMesh : public QueryObject {
    GeneralTriangleMesh(const coord_t *vtx_x,
                        const coord_t *vtx_y,
                        const coord_t *vtx_z,
                        const size_t   vtx_stride,
                        const index_t *idx_x,
                        const index_t *idx_y,
                        const index_t *idx_z,
                        const size_t   idx_stride,
                        const size_t   numTriangles);

    const coord_t *const vtx_x;
    const coord_t *const vtx_y;
    const coord_t *const vtx_z;
    const size_t   vtx_stride;
    const index_t *const idx_x;
    const index_t *const idx_y;
    const index_t *const idx_z;
    const size_t   idx_stride;
    const size_t   numTriangles;
    
    virtual ~GeneralTriangleMesh()
    {
    }

    virtual size_t numPrimitives() const { return numTriangles; }

    virtual void initBuildPrim(BuildPrim &bp, const size_t primID) const
    {
      const size_t i0 = this->idx_x[primID*idx_stride];
      const size_t i1 = this->idx_y[primID*idx_stride];
      const size_t i2 = this->idx_z[primID*idx_stride];

      const vec3fa v0(this->vtx_x[i0*vtx_stride],
                      this->vtx_y[i0*vtx_stride],
                      this->vtx_z[i0*vtx_stride]);
      const vec3fa v1(this->vtx_x[i1*vtx_stride],
                      this->vtx_y[i1*vtx_stride],
                      this->vtx_z[i1*vtx_stride]);
      const vec3fa v2(this->vtx_x[i2*vtx_stride],
                      this->vtx_y[i2*vtx_stride],
                      this->vtx_z[i2*vtx_stride]); 
      const vec3f lo = min(min(v0,v1),v2);
      const vec3f hi = max(max(v0,v1),v2);
      bp.lower = lo;
      bp.upper = hi;
      // bp.geomID = primID >> 32;
      // bp.primID = primID;
    }

    /* based on dave eberly's test */
    virtual void updateIfCloser(QueryResult &result,
                                const vec3f &P,
                                const size_t primID)
    {
      const size_t i0 = this->idx_x[primID*idx_stride];
      const size_t i1 = this->idx_y[primID*idx_stride];
      const size_t i2 = this->idx_z[primID*idx_stride];

      const vec3f A(this->vtx_x[i0*vtx_stride],
                    this->vtx_y[i0*vtx_stride],
                    this->vtx_z[i0*vtx_stride]);
      const vec3f B(this->vtx_x[i1*vtx_stride],
                    this->vtx_y[i1*vtx_stride],
                    this->vtx_z[i1*vtx_stride]);
      const vec3f C(this->vtx_x[i2*vtx_stride],
                    this->vtx_y[i2*vtx_stride],
                    this->vtx_z[i2*vtx_stride]); 
      const vec3f PP   = closestPoint(A,B,C,P);
      const float dist = length(PP-P);
      if (dist >= result.distance) return;

      result.distance = dist;
      result.point    = PP;
      result.primID   = primID;
    }
  };


  template<typename coord_t, typename index_t>
  GeneralTriangleMesh<coord_t,index_t>::GeneralTriangleMesh(const coord_t *vtx_x,
                                                            const coord_t *vtx_y,
                                                            const coord_t *vtx_z,
                                                            const size_t   vtx_stride,
                                                            const index_t *idx_x,
                                                            const index_t *idx_y,
                                                            const index_t *idx_z,
                                                            const size_t   idx_stride,
                                                            const size_t   numTriangles)
  : vtx_x(vtx_x),
    vtx_y(vtx_y),
    vtx_z(vtx_z),
    vtx_stride(vtx_stride),
    idx_x(idx_x),
    idx_y(idx_y),
    idx_z(idx_z),
    idx_stride(idx_stride),
    numTriangles(numTriangles)
  {
    bvh.build(this);
  }

  extern "C"
  distance_query_scene rtdqNewTriangleMeshfi(const float   *vertex_x,
                                             const float   *vertex_y,
                                             const float   *vertex_z,
                                             const size_t   vertex_stride,
                                             const int32_t *index_x,
                                             const int32_t *index_y,
                                             const int32_t *index_z,
                                             const size_t   index_stride,
                                             const size_t    numTriangles)
  {
    return (distance_query_scene)
      new GeneralTriangleMesh<float,int>(vertex_x,vertex_y,vertex_z,vertex_stride,
                                         index_x,index_y,index_z,index_stride,
                                         numTriangles);
  }

  inline float computeDistance(const BVH::Node *node, const vec3f &P)
  {
    const vec3f Pclamped = min(max(P,node->lower),node->upper);
    return length(P-Pclamped);
  }
  
  // inline vfloat<4> computeDistance(const BVH4::AlignedNode *node, const vec3f &P)
  // {
  //   // vfloat<4> clamped_x = min(max(P.x,node->lower_x),node->upper_x);
  //   // vfloat<4> clamped_y = min(max(P.y,node->lower_y),node->upper_y);
  //   // vfloat<4> clamped_z = min(max(P.z,node->lower_z),node->upper_z);

  //   // vfloat<4> dx = clamped_x - P.x;
  //   // vfloat<4> dy = clamped_y - P.y;
  //   // vfloat<4> dz = clamped_z - P.z;
                 
  //   // return dx*dx + dy*dy + dz*dz;
  // }
  
  void oneQuery(QueryResult &result,
                QueryObject *qo,
                const vec3f &point)
  {
    // std::cout << "=======================================================" << std::endl;
    // std::cout << "new query " << point << std::endl;
    result.distance = std::numeric_limits<float>::infinity();
    result.primID   = -1;
  
    // // we already type-checked this before tihs fct ever got called,
    // // so this is safe:
    // BVH4 *bvh4 = (BVH4*)((Accel*)qo->scene)->intersectors.ptr;
    
    std::priority_queue<std::pair<float,const BVH::Node *>,
                        std::vector<std::pair<float,const BVH::Node *>>,
                        std::greater<std::pair<float,const BVH::Node *>>
                        > queue;
    // push sentinel
    queue.push(std::pair<float,const BVH::Node *>
               (std::numeric_limits<float>::infinity(),NULL));
    
    const std::vector<BVH::Node> &nodeList = qo->bvh.nodeList;
    const BVH::Node *node = &nodeList[0];
    while (1) {
      // std::cout << "--------------------------------------------" << std::endl;
      // PRINT(point);
      // PRINT((box3fa &)*node);
      // PRINT(computeDistance(node,point));
      if (!node->isLeaf) {
        // this is a inner node ...
        const BVH::Node *child0 = &nodeList[node->child+0];
        const BVH::Node *child1 = &nodeList[node->child+1];
        const float dist0 = computeDistance(child0,point);
        const float dist1 = computeDistance(child1,point);
        // PRINT(dist0);
        // PRINT(dist1);

        if (dist0 == 0.f) {
          node = child0;
          if (dist1 < result.distance)
            queue.push(std::pair<float,const BVH::Node*>(dist1,child1));
          continue;
        } else if (dist1 == 0.f) {
          node = child1;
          if (dist0 < result.distance)
            queue.push(std::pair<float,const BVH::Node*>(dist0,child0));
          continue;
        }

        if (dist0 < result.distance)
          queue.push(std::pair<float,const BVH::Node*>(dist0,child0));
        if (dist1 < result.distance)
          queue.push(std::pair<float,const BVH::Node*>(dist1,child1));
      } else {
        /// this is a leaf node
        // std::cout << "**** testing triangle " << node->child << " *****" << std::endl;
        qo->updateIfCloser(result,point,node->child);
      }

      // any more candidates in queue?
      if (queue.empty()) break;

      // closest candidate is already too far?
      // PRINT(queue.top().first);
      // PRINT(result.distance);
      if (queue.top().first >= result.distance) break;

      // closest candidate might be closer: pop it and use it
      node = queue.top().second;
      queue.pop();
    }
  }
  
  void computeQuery(QueryResult *resultArray,
                    QueryObject *qo,
                    const vec3f *const point,  const size_t numPoints)
  {
  }

  extern "C"
  void rtdqComputeClosestPoints(distance_query_scene scene,
                                float   *out_closest_point_pos_x,
                                float   *out_closest_point_pos_y,
                                float   *out_closest_point_pos_z,
                                size_t   out_closest_point_pos_stride,
                                float   *out_closest_point_dist,
                                size_t   out_closest_point_dist_stride,
                                int32_t *out_closest_point_primID,
                                size_t   out_closest_point_primID_stride,
                                const float *in_query_point_x,
                                const float *in_query_point_y,
                                const float *in_query_point_z,
                                const size_t in_query_point_stride,
                                const size_t numQueryPoints)
  {
    QueryObject *qo = (QueryObject *)scene;
    if (!qo)
      return;
    // AccelData *accel = ((Accel*)qo->scene)->intersectors.ptr;
    // if (!accel)
    //   return;
    // if (accel->type != AccelData::TY_BVH4)
    //   return;

    for (size_t i=0;i<numQueryPoints;i++) {
      QueryResult qr;
      oneQuery(qr,qo,vec3f(in_query_point_x[i*in_query_point_stride],
                           in_query_point_y[i*in_query_point_stride],
                           in_query_point_z[i*in_query_point_stride]));
      if (out_closest_point_pos_x)
        out_closest_point_pos_x[i*out_closest_point_pos_stride] = qr.point.x;
      if (out_closest_point_pos_y)
        out_closest_point_pos_y[i*out_closest_point_pos_stride] = qr.point.y;
      if (out_closest_point_pos_z)
        out_closest_point_pos_z[i*out_closest_point_pos_stride] = qr.point.z;
      if (out_closest_point_primID)
        out_closest_point_primID[i*out_closest_point_primID_stride] = qr.primID;
      if (out_closest_point_dist)
        out_closest_point_dist[i*out_closest_point_dist_stride] = qr.distance;
    }
    // throw std::runtime_error("not implemented");
  }
  
  
  /*! destroy a scene created with rtdqNew...() */
  extern "C"
  void rtdqDestroy(distance_query_scene scene)
  {
    if (scene) delete (QueryObject*)scene;
  }

  
}
 
