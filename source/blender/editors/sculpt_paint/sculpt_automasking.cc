/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_blenlib.h"
#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_math_base_safe.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_task.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_brush.hh"
#include "BKE_colortools.h"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"
#include "BKE_scene.h"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.h"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "paint_intern.hh"
#include "sculpt_intern.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>

namespace blender::ed::sculpt_paint::auto_mask {

Cache *active_cache_get(SculptSession *ss)
{
  if (ss->cache) {
    return ss->cache->automasking;
  }
  if (ss->filter_cache) {
    return ss->filter_cache->automasking;
  }
  return nullptr;
}

bool mode_enabled(const Sculpt *sd, const Brush *br, const eAutomasking_flag mode)
{
  int automasking = sd->automasking_flags;

  if (br) {
    automasking |= br->automasking_flags;
  }

  return (eAutomasking_flag)automasking & mode;
}

bool is_enabled(const Sculpt *sd, const SculptSession *ss, const Brush *br)
{
  if (ss && br && dyntopo::stroke_is_dyntopo(ss, br)) {
    return false;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_TOPOLOGY)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_FACE_SETS)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_BOUNDARY_EDGES)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_BRUSH_NORMAL)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_VIEW_NORMAL)) {
    return true;
  }
  if (mode_enabled(sd, br, BRUSH_AUTOMASKING_CAVITY_ALL)) {
    return true;
  }

  return false;
}

static int sculpt_automasking_mode_effective_bits(const Sculpt *sculpt, const Brush *brush)
{
  if (brush) {
    int flags = sculpt->automasking_flags | brush->automasking_flags;

    /* Check if we are using brush cavity settings. */
    if (brush->automasking_flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
      flags &= ~(BRUSH_AUTOMASKING_CAVITY_ALL | BRUSH_AUTOMASKING_CAVITY_USE_CURVE |
                 BRUSH_AUTOMASKING_CAVITY_NORMAL);
      flags |= brush->automasking_flags;
    }
    else if (sculpt->automasking_flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
      flags &= ~(BRUSH_AUTOMASKING_CAVITY_ALL | BRUSH_AUTOMASKING_CAVITY_USE_CURVE |
                 BRUSH_AUTOMASKING_CAVITY_NORMAL);
      flags |= sculpt->automasking_flags;
    }

    return flags;
  }
  return sculpt->automasking_flags;
}

bool needs_normal(const SculptSession * /*ss*/, const Sculpt *sculpt, const Brush *brush)
{
  int flags = sculpt_automasking_mode_effective_bits(sculpt, brush);

  return flags & (BRUSH_AUTOMASKING_BRUSH_NORMAL | BRUSH_AUTOMASKING_VIEW_NORMAL);
}

static float sculpt_automasking_normal_calc(SculptSession *ss,
                                            PBVHVertRef vertex,
                                            float3 &normal,
                                            float limit_lower,
                                            float limit_upper,
                                            NodeData *automask_data)
{
  float3 normal_v;

  if (automask_data->have_orig_data) {
    normal_v = automask_data->orig_data.no;
  }
  else {
    SCULPT_vertex_normal_get(ss, vertex, normal_v);
  }

  float angle = safe_acosf(dot_v3v3(normal, normal_v));

  /* note that limit is pre-divided by M_PI */

  if (angle > limit_lower && angle < limit_upper) {
    float t = 1.0f - (angle - limit_lower) / (limit_upper - limit_lower);

    /* smoothstep */
    t = t * t * (3.0 - 2.0 * t);

    return t;
  }
  if (angle > limit_upper) {
    return 0.0f;
  }

  return 1.0f;
}

static bool sculpt_automasking_is_constrained_by_radius(const Brush *br)
{
  if (br == nullptr) {
    return false;
  }

  /* 2D falloff is not constrained by radius. */
  if (br->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    return false;
  }

  if (ELEM(br->sculpt_tool, SCULPT_TOOL_GRAB, SCULPT_TOOL_THUMB, SCULPT_TOOL_ROTATE)) {
    return true;
  }
  return false;
}

static bool SCULPT_automasking_needs_factors_cache(const Sculpt *sd, const Brush *brush)
{

  const int automasking_flags = sculpt_automasking_mode_effective_bits(sd, brush);

  if (automasking_flags & BRUSH_AUTOMASKING_TOPOLOGY && brush &&
      sculpt_automasking_is_constrained_by_radius(brush))
  {
    return true;
  }

  if (automasking_flags & (BRUSH_AUTOMASKING_BOUNDARY_EDGES |
                           BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS | BRUSH_AUTOMASKING_VIEW_NORMAL))
  {
    return brush && brush->automasking_boundary_edges_propagation_steps != 1;
  }
  return false;
}

static float automasking_brush_normal_factor(Cache *automasking,
                                             SculptSession *ss,
                                             PBVHVertRef vertex,
                                             NodeData *automask_data)
{
  float falloff = automasking->settings.start_normal_falloff * M_PI;
  float3 initial_normal;

  if (ss->cache) {
    initial_normal = ss->cache->initial_normal;
  }
  else {
    initial_normal = ss->filter_cache->initial_normal;
  }

  return sculpt_automasking_normal_calc(ss,
                                        vertex,
                                        initial_normal,
                                        automasking->settings.start_normal_limit - falloff * 0.5f,
                                        automasking->settings.start_normal_limit + falloff * 0.5f,
                                        automask_data);
}

static float automasking_view_normal_factor(Cache *automasking,
                                            SculptSession *ss,
                                            PBVHVertRef vertex,
                                            NodeData *automask_data)
{
  float falloff = automasking->settings.view_normal_falloff * M_PI;

  float3 view_normal;

  if (ss->cache) {
    view_normal = ss->cache->view_normal;
  }
  else {
    view_normal = ss->filter_cache->view_normal;
  }

  return sculpt_automasking_normal_calc(ss,
                                        vertex,
                                        view_normal,
                                        automasking->settings.view_normal_limit,
                                        automasking->settings.view_normal_limit + falloff,
                                        automask_data);
}

static float automasking_view_occlusion_factor(Cache *automasking,
                                               SculptSession *ss,
                                               PBVHVertRef vertex,
                                               uchar stroke_id,
                                               NodeData * /*automask_data*/)
{
  char f = *(char *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_occlusion);

  if (stroke_id != automasking->current_stroke_id) {
    f = *(char *)SCULPT_vertex_attr_get(
        vertex,
        ss->attrs.automasking_occlusion) = SCULPT_vertex_is_occluded(ss, vertex, true) ? 2 : 1;
  }

  return f == 2;
}

/* Updates vertex stroke id. */
static float automasking_factor_end(SculptSession *ss,
                                    Cache *automasking,
                                    PBVHVertRef vertex,
                                    float value)
{
  if (ss->attrs.automasking_stroke_id) {
    *(uchar *)SCULPT_vertex_attr_get(
        vertex, ss->attrs.automasking_stroke_id) = automasking->current_stroke_id;
  }

  return value;
}

static float sculpt_cavity_calc_factor(Cache *automasking, float factor)
{
  float sign = signf(factor);

  factor = fabsf(factor) * automasking->settings.cavity_factor * 50.0f;

  factor = factor * sign * 0.5f + 0.5f;
  CLAMP(factor, 0.0f, 1.0f);

  return (automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_INVERTED) ? 1.0f - factor :
                                                                             factor;
}

struct CavityBlurVert {
  PBVHVertRef vertex;
  float dist;
  int depth;

  CavityBlurVert(PBVHVertRef vertex_, float dist_, int depth_)
      : vertex(vertex_), dist(dist_), depth(depth_)
  {
  }

  CavityBlurVert() = default;
};

static void sculpt_calc_blurred_cavity(SculptSession *ss,
                                       Cache *automasking,
                                       int steps,
                                       PBVHVertRef vertex)
{
  float3 sno1(0.0f);
  float3 sno2(0.0f);
  float3 sco1(0.0f);
  float3 sco2(0.0f);
  float len1_sum = 0.0f;
  int sco1_len = 0, sco2_len = 0;

  /* Steps starts at 1, but API and user interface
   * are zero-based.
   */
  steps++;

  Vector<CavityBlurVert, 64> queue;
  Set<int64_t, 64> visit;

  int start = 0, end = 0;

  queue.resize(64);

  CavityBlurVert initial(vertex, 0.0f, 0);

  visit.add_new(vertex.i);
  queue[0] = initial;
  end = 1;

  const float *co1 = SCULPT_vertex_co_get(ss, vertex);

  while (start != end) {
    CavityBlurVert &blurvert = queue[start];
    PBVHVertRef v = blurvert.vertex;
    start = (start + 1) % queue.size();

    float3 no;

    const float *co = SCULPT_vertex_co_get(ss, v);
    SCULPT_vertex_normal_get(ss, v, no);

    float centdist = len_v3v3(co, co1);

    sco1 += co;
    sno1 += no;
    len1_sum += centdist;
    sco1_len++;

    if (blurvert.depth < steps) {
      sco2 += co;
      sno2 += no;
      sco2_len++;
    }

    if (blurvert.depth >= steps) {
      continue;
    }

    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, v, ni) {
      PBVHVertRef v2 = ni.vertex;

      if (visit.contains(v2.i)) {
        continue;
      }

      float dist = len_v3v3(SCULPT_vertex_co_get(ss, v2), SCULPT_vertex_co_get(ss, v));

      visit.add_new(v2.i);
      CavityBlurVert blurvert2(v2, dist, blurvert.depth + 1);

      int nextend = (end + 1) % queue.size();

      if (nextend == start) {
        int oldsize = queue.size();

        queue.resize(queue.size() << 1);

        if (end < start) {
          int n = oldsize - start;

          for (int i = 0; i < n; i++) {
            queue[queue.size() - n + i] = queue[i + start];
          }

          start = queue.size() - n;
        }
      }

      queue[end] = blurvert2;
      end = (end + 1) % queue.size();
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  }

  BLI_assert(sco1_len != sco2_len);

  if (!sco1_len) {
    sco1 = SCULPT_vertex_co_get(ss, vertex);
  }
  else {
    sco1 /= float(sco1_len);
    len1_sum /= sco1_len;
  }

  if (!sco2_len) {
    sco2 = SCULPT_vertex_co_get(ss, vertex);
  }
  else {
    sco2 /= float(sco2_len);
  }

  normalize_v3(sno1);
  if (dot_v3v3(sno1, sno1) == 0.0f) {
    SCULPT_vertex_normal_get(ss, vertex, sno1);
  }

  normalize_v3(sno2);
  if (dot_v3v3(sno2, sno2) == 0.0f) {
    SCULPT_vertex_normal_get(ss, vertex, sno2);
  }

  float3 vec = sco1 - sco2;
  float factor_sum = dot_v3v3(vec, sno2) / len1_sum;

  factor_sum = sculpt_cavity_calc_factor(automasking, factor_sum);

  *(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_cavity) = factor_sum;
}

int settings_hash(Object *ob, Cache *automasking)
{
  SculptSession *ss = ob->sculpt;

  int hash;
  int totvert = SCULPT_vertex_count_get(ss);

  hash = BLI_hash_int(automasking->settings.flags);
  hash = BLI_hash_int_2d(hash, totvert);

  if (automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
    hash = BLI_hash_int_2d(hash, automasking->settings.cavity_blur_steps);
    hash = BLI_hash_int_2d(hash, *reinterpret_cast<uint *>(&automasking->settings.cavity_factor));

    if (automasking->settings.cavity_curve) {
      CurveMap *cm = automasking->settings.cavity_curve->cm;

      for (int i = 0; i < cm->totpoint; i++) {
        hash = BLI_hash_int_2d(hash, *reinterpret_cast<uint *>(&cm->curve[i].x));
        hash = BLI_hash_int_2d(hash, *reinterpret_cast<uint *>(&cm->curve[i].y));
        hash = BLI_hash_int_2d(hash, uint(cm->curve[i].flag));
        hash = BLI_hash_int_2d(hash, uint(cm->curve[i].shorty));
      }
    }
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_FACE_SETS) {
    hash = BLI_hash_int_2d(hash, automasking->settings.initial_face_set);
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_VIEW_NORMAL) {
    hash = BLI_hash_int_2d(hash,
                           *reinterpret_cast<uint *>(&automasking->settings.view_normal_falloff));
    hash = BLI_hash_int_2d(hash,
                           *reinterpret_cast<uint *>(&automasking->settings.view_normal_limit));
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_BRUSH_NORMAL) {
    hash = BLI_hash_int_2d(hash,
                           *reinterpret_cast<uint *>(&automasking->settings.start_normal_falloff));
    hash = BLI_hash_int_2d(hash,
                           *reinterpret_cast<uint *>(&automasking->settings.start_normal_limit));
  }

  return hash;
}

static float sculpt_automasking_cavity_factor(Cache *automasking,
                                              SculptSession *ss,
                                              PBVHVertRef vertex)
{
  uchar stroke_id = *(uchar *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_stroke_id);

  if (stroke_id != automasking->current_stroke_id) {
    sculpt_calc_blurred_cavity(ss, automasking, automasking->settings.cavity_blur_steps, vertex);
  }

  float factor = *(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_cavity);
  bool inverted = automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_INVERTED;

  if ((automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL) &&
      (automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_USE_CURVE))
  {
    factor = inverted ? 1.0f - factor : factor;
    factor = BKE_curvemapping_evaluateF(automasking->settings.cavity_curve, 0, factor);
    factor = inverted ? 1.0f - factor : factor;
  }

  return factor;
}

float factor_get(Cache *automasking, SculptSession *ss, PBVHVertRef vert, NodeData *automask_data)
{
  if (!automasking || vert.i == PBVH_REF_NONE) {
    return 1.0f;
  }

  float mask = 1.0f;

  /* Since brush normal mode depends on the current mirror symmetry pass
   * it is not folded into the factor cache (when it exists). */
  if ((ss->cache || ss->filter_cache) &&
      (automasking->settings.flags & BRUSH_AUTOMASKING_BRUSH_NORMAL))
  {
    mask *= automasking_brush_normal_factor(automasking, ss, vert, automask_data);
  }

  /* If the cache is initialized with valid info, use the cache. This is used when the
   * automasking information can't be computed in real time per vertex and needs to be
   * initialized for the whole mesh when the stroke starts. */
  if (ss->attrs.automasking_factor) {
    float factor = *(float *)SCULPT_vertex_attr_get(vert, ss->attrs.automasking_factor);

    if (automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
      factor *= sculpt_automasking_cavity_factor(automasking, ss, vert);
    }

    return automasking_factor_end(ss, automasking, vert, factor * mask);
  }

  uchar stroke_id = ss->attrs.automasking_stroke_id ?
                        *(uchar *)SCULPT_vertex_attr_get(vert, ss->attrs.automasking_stroke_id) :
                        -1;

  bool do_occlusion = (automasking->settings.flags &
                       (BRUSH_AUTOMASKING_VIEW_OCCLUSION | BRUSH_AUTOMASKING_VIEW_NORMAL)) ==
                      (BRUSH_AUTOMASKING_VIEW_OCCLUSION | BRUSH_AUTOMASKING_VIEW_NORMAL);
  if (do_occlusion &&
      automasking_view_occlusion_factor(automasking, ss, vert, stroke_id, automask_data))
  {
    return automasking_factor_end(ss, automasking, vert, 0.0f);
  }

  if (!automasking->settings.topology_use_brush_limit &&
      automasking->settings.flags & BRUSH_AUTOMASKING_TOPOLOGY &&
      SCULPT_vertex_island_get(ss, vert) != automasking->settings.initial_island_nr)
  {
    return 0.0f;
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_FACE_SETS) {
    if (!face_set::vert_has_face_set(ss, vert, automasking->settings.initial_face_set)) {
      return 0.0f;
    }
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_BOUNDARY_EDGES) {
    if (SCULPT_vertex_is_boundary(ss, vert)) {
      return 0.0f;
    }
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS) {
    bool ignore = ss->cache && ss->cache->brush &&
                  ss->cache->brush->sculpt_tool == SCULPT_TOOL_DRAW_FACE_SETS &&
                  face_set::vert_face_set_get(ss, vert) == ss->cache->paint_face_set;

    if (!ignore && !face_set::vert_has_unique_face_set(ss, vert)) {
      return 0.0f;
    }
  }

  if ((ss->cache || ss->filter_cache) &&
      (automasking->settings.flags & BRUSH_AUTOMASKING_VIEW_NORMAL))
  {
    mask *= automasking_view_normal_factor(automasking, ss, vert, automask_data);
  }

  if (automasking->settings.flags & BRUSH_AUTOMASKING_CAVITY_ALL) {
    mask *= sculpt_automasking_cavity_factor(automasking, ss, vert);
  }

  return automasking_factor_end(ss, automasking, vert, mask);
}

void cache_free(Cache *automasking)
{
  if (!automasking) {
    return;
  }

  MEM_SAFE_FREE(automasking);
}

struct AutomaskFloodFillData {
  float radius;
  bool use_radius;
  float location[3];
  char symm;
};

static bool automask_floodfill_cb(
    SculptSession *ss, PBVHVertRef from_v, PBVHVertRef to_v, bool /*is_duplicate*/, void *userdata)
{
  AutomaskFloodFillData *data = (AutomaskFloodFillData *)userdata;

  *(float *)SCULPT_vertex_attr_get(to_v, ss->attrs.automasking_factor) = 1.0f;
  *(float *)SCULPT_vertex_attr_get(from_v, ss->attrs.automasking_factor) = 1.0f;
  return (!data->use_radius ||
          SCULPT_is_vertex_inside_brush_radius_symm(
              SCULPT_vertex_co_get(ss, to_v), data->location, data->radius, data->symm));
}

static void SCULPT_topology_automasking_init(Sculpt *sd, Object *ob)
{
  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  const int totvert = SCULPT_vertex_count_get(ss);
  for (int i : IndexRange(totvert)) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    (*(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_factor)) = 0.0f;
  }

  /* Flood fill automask to connected vertices. Limited to vertices inside
   * the brush radius if the tool requires it. */
  SculptFloodFill flood;
  flood_fill::init_fill(ss, &flood);
  const float radius = ss->cache ? ss->cache->radius : FLT_MAX;
  flood_fill::add_active(sd, ob, ss, &flood, radius);

  AutomaskFloodFillData fdata = {0};

  fdata.radius = radius;
  fdata.use_radius = ss->cache && sculpt_automasking_is_constrained_by_radius(brush);
  fdata.symm = SCULPT_mesh_symmetry_xyz_get(ob);

  copy_v3_v3(fdata.location, SCULPT_active_vertex_co_get(ss));
  flood_fill::execute(ss, &flood, automask_floodfill_cb, &fdata);
  flood_fill::free_fill(&flood);
}

static void sculpt_face_sets_automasking_init(Sculpt *sd, Object *ob)
{
  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  if (!is_enabled(sd, ss, brush)) {
    return;
  }

  int tot_vert = SCULPT_vertex_count_get(ss);
  int active_face_set = face_set::active_face_set_get(ss);
  for (int i : IndexRange(tot_vert)) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    if (!face_set::vert_has_face_set(ss, vertex, active_face_set)) {
      *(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_factor) = 0.0f;
    }
  }
}

#define EDGE_DISTANCE_INF -1

static void boundary_automasking_init(Object *ob,
                                      eBoundaryAutomaskMode mode,
                                      int propagation_steps)
{
  SculptSession *ss = ob->sculpt;

  const int totvert = SCULPT_vertex_count_get(ss);
  int *edge_distance = (int *)MEM_callocN(sizeof(int) * totvert, "automask_factor");

  for (int i : IndexRange(totvert)) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    edge_distance[i] = EDGE_DISTANCE_INF;
    switch (mode) {
      case AUTOMASK_INIT_BOUNDARY_EDGES:
        if (SCULPT_vertex_is_boundary(ss, vertex)) {
          edge_distance[i] = 0;
        }
        break;
      case AUTOMASK_INIT_BOUNDARY_FACE_SETS:
        if (!face_set::vert_has_unique_face_set(ss, vertex)) {
          edge_distance[i] = 0;
        }
        break;
    }
  }

  for (int propagation_it : IndexRange(propagation_steps)) {
    for (int i : IndexRange(totvert)) {
      PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

      if (edge_distance[i] != EDGE_DISTANCE_INF) {
        continue;
      }
      SculptVertexNeighborIter ni;
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
        if (edge_distance[ni.index] == propagation_it) {
          edge_distance[i] = propagation_it + 1;
        }
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    }
  }

  for (int i : IndexRange(totvert)) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    if (edge_distance[i] == EDGE_DISTANCE_INF) {
      continue;
    }
    const float p = 1.0f - (float(edge_distance[i]) / float(propagation_steps));
    const float edge_boundary_automask = pow2f(p);

    *(float *)SCULPT_vertex_attr_get(
        vertex, ss->attrs.automasking_factor) *= (1.0f - edge_boundary_automask);
  }

  MEM_SAFE_FREE(edge_distance);
}

static void SCULPT_automasking_cache_settings_update(Cache *automasking,
                                                     SculptSession *ss,
                                                     Sculpt *sd,
                                                     Brush *brush)
{
  automasking->settings.flags = sculpt_automasking_mode_effective_bits(sd, brush);
  automasking->settings.initial_face_set = face_set::active_face_set_get(ss);

  automasking->settings.view_normal_limit = sd->automasking_view_normal_limit;
  automasking->settings.view_normal_falloff = sd->automasking_view_normal_falloff;
  automasking->settings.start_normal_limit = sd->automasking_start_normal_limit;
  automasking->settings.start_normal_falloff = sd->automasking_start_normal_falloff;

  if (brush && (brush->automasking_flags & BRUSH_AUTOMASKING_CAVITY_ALL)) {
    automasking->settings.cavity_curve = brush->automasking_cavity_curve;
    automasking->settings.cavity_factor = brush->automasking_cavity_factor;
    automasking->settings.cavity_blur_steps = brush->automasking_cavity_blur_steps;
  }
  else {
    automasking->settings.cavity_curve = sd->automasking_cavity_curve;
    automasking->settings.cavity_factor = sd->automasking_cavity_factor;
    automasking->settings.cavity_blur_steps = sd->automasking_cavity_blur_steps;
  }
}

static void sculpt_normal_occlusion_automasking_fill(Cache *automasking,
                                                     Object *ob,
                                                     eAutomasking_flag mode)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);

  /* No need to build original data since this is only called at the beginning of strokes. */
  NodeData nodedata;
  nodedata.have_orig_data = false;

  for (int i = 0; i < totvert; i++) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    float f = *(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_factor);

    if (int(mode) & BRUSH_AUTOMASKING_VIEW_NORMAL) {
      if (int(mode) & BRUSH_AUTOMASKING_VIEW_OCCLUSION) {
        f *= automasking_view_occlusion_factor(automasking, ss, vertex, -1, &nodedata);
      }

      f *= automasking_view_normal_factor(automasking, ss, vertex, &nodedata);
    }

    if (ss->attrs.automasking_stroke_id) {
      *(uchar *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_stroke_id) = ss->stroke_id;
    }

    *(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_factor) = f;
  }
}

bool tool_can_reuse_automask(int sculpt_tool)
{
  return ELEM(sculpt_tool,
              SCULPT_TOOL_PAINT,
              SCULPT_TOOL_SMEAR,
              SCULPT_TOOL_MASK,
              SCULPT_TOOL_DRAW_FACE_SETS);
}

Cache *cache_init(Sculpt *sd, Brush *brush, Object *ob)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);

  if (!is_enabled(sd, ss, brush)) {
    return nullptr;
  }

  Cache *automasking = (Cache *)MEM_callocN(sizeof(Cache), "automasking cache");
  SCULPT_automasking_cache_settings_update(automasking, ss, sd, brush);
  SCULPT_boundary_info_ensure(ob);

  automasking->current_stroke_id = ss->stroke_id;

  bool use_stroke_id = false;
  int mode = sculpt_automasking_mode_effective_bits(sd, brush);

  if (mode & BRUSH_AUTOMASKING_TOPOLOGY && ss->active_vertex.i != PBVH_REF_NONE) {
    SCULPT_topology_islands_ensure(ob);
    automasking->settings.initial_island_nr = SCULPT_vertex_island_get(ss, ss->active_vertex);
  }

  if ((mode & BRUSH_AUTOMASKING_VIEW_OCCLUSION) && (mode & BRUSH_AUTOMASKING_VIEW_NORMAL)) {
    use_stroke_id = true;

    if (!ss->attrs.automasking_occlusion) {
      SculptAttributeParams params = {0};
      ss->attrs.automasking_occlusion = BKE_sculpt_attribute_ensure(
          ob,
          ATTR_DOMAIN_POINT,
          CD_PROP_INT8,
          SCULPT_ATTRIBUTE_NAME(automasking_occlusion),
          &params);
    }
  }

  if (mode & BRUSH_AUTOMASKING_CAVITY_ALL) {
    use_stroke_id = true;

    if (mode_enabled(sd, brush, BRUSH_AUTOMASKING_CAVITY_USE_CURVE)) {
      if (brush) {
        BKE_curvemapping_init(brush->automasking_cavity_curve);
      }

      BKE_curvemapping_init(sd->automasking_cavity_curve);
    }

    if (!ss->attrs.automasking_cavity) {
      SculptAttributeParams params = {0};
      ss->attrs.automasking_cavity = BKE_sculpt_attribute_ensure(
          ob,
          ATTR_DOMAIN_POINT,
          CD_PROP_FLOAT,
          SCULPT_ATTRIBUTE_NAME(automasking_cavity),
          &params);
    }
  }

  if (use_stroke_id) {
    SCULPT_stroke_id_ensure(ob);

    bool have_occlusion = (mode & BRUSH_AUTOMASKING_VIEW_OCCLUSION) &&
                          (mode & BRUSH_AUTOMASKING_VIEW_NORMAL);

    if (brush && auto_mask::tool_can_reuse_automask(brush->sculpt_tool) && !have_occlusion) {
      int hash = settings_hash(ob, automasking);

      if (hash == ss->last_automasking_settings_hash) {
        automasking->current_stroke_id = ss->last_automask_stroke_id;
        automasking->can_reuse_mask = true;
      }
    }

    if (!automasking->can_reuse_mask) {
      ss->last_automask_stroke_id = ss->stroke_id;
    }
  }

  if (!SCULPT_automasking_needs_factors_cache(sd, brush)) {
    if (ss->attrs.automasking_factor) {
      BKE_sculpt_attribute_destroy(ob, ss->attrs.automasking_factor);
    }
    return automasking;
  }

  SculptAttributeParams params = {0};
  params.stroke_only = true;

  ss->attrs.automasking_factor = BKE_sculpt_attribute_ensure(
      ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, SCULPT_ATTRIBUTE_NAME(automasking_factor), &params);

  float initial_value;

  /* Topology, boundary and boundary face sets build up the mask
   * from zero which other modes can subtract from.  If none of them are
   * enabled initialize to 1.
   */
  if (!(mode & BRUSH_AUTOMASKING_TOPOLOGY)) {
    initial_value = 1.0f;
  }
  else {
    initial_value = 0.0f;
  }

  for (int i : IndexRange(totvert)) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    (*(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.automasking_factor)) = initial_value;
  }

  const int boundary_propagation_steps = brush ?
                                             brush->automasking_boundary_edges_propagation_steps :
                                             1;

  /* Additive modes. */
  if (mode_enabled(sd, brush, BRUSH_AUTOMASKING_TOPOLOGY)) {
    SCULPT_vertex_random_access_ensure(ss);

    automasking->settings.topology_use_brush_limit = sculpt_automasking_is_constrained_by_radius(
        brush);
    SCULPT_topology_automasking_init(sd, ob);
  }

  if (mode_enabled(sd, brush, BRUSH_AUTOMASKING_FACE_SETS)) {
    SCULPT_vertex_random_access_ensure(ss);
    sculpt_face_sets_automasking_init(sd, ob);
  }

  if (mode_enabled(sd, brush, BRUSH_AUTOMASKING_BOUNDARY_EDGES)) {
    SCULPT_vertex_random_access_ensure(ss);
    boundary_automasking_init(ob, AUTOMASK_INIT_BOUNDARY_EDGES, boundary_propagation_steps);
  }
  if (mode_enabled(sd, brush, BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS)) {
    SCULPT_vertex_random_access_ensure(ss);
    boundary_automasking_init(ob, AUTOMASK_INIT_BOUNDARY_FACE_SETS, boundary_propagation_steps);
  }

  /* Subtractive modes. */
  int normal_bits = sculpt_automasking_mode_effective_bits(sd, brush) &
                    (BRUSH_AUTOMASKING_VIEW_NORMAL | BRUSH_AUTOMASKING_VIEW_OCCLUSION);

  if (normal_bits) {
    sculpt_normal_occlusion_automasking_fill(automasking, ob, (eAutomasking_flag)normal_bits);
  }

  return automasking;
}

}  // namespace blender::ed::sculpt_paint::auto_mask
