/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from the Blender Alembic importer implementation.
 * Modifications Copyright 2021 Tangent Animation and
 * NVIDIA Corporation. All rights reserved. */

#include "usd_reader_mesh.h"
#include "usd_reader_material.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mesh.hh"
#include "BKE_object.h"

#include "BLI_math.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"

#include "DNA_customdata_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "MEM_guardedalloc.h"

#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <iostream>

namespace usdtokens {
/* Materials */
static const pxr::TfToken st("st", pxr::TfToken::Immortal);
static const pxr::TfToken UVMap("UVMap", pxr::TfToken::Immortal);
static const pxr::TfToken Cd("Cd", pxr::TfToken::Immortal);
static const pxr::TfToken displayColor("displayColor", pxr::TfToken::Immortal);
static const pxr::TfToken normalsPrimvar("normals", pxr::TfToken::Immortal);
}  // namespace usdtokens

namespace utils {

static pxr::UsdShadeMaterial compute_bound_material(const pxr::UsdPrim &prim)
{
  pxr::UsdShadeMaterialBindingAPI api = pxr::UsdShadeMaterialBindingAPI(prim);

  /* Compute generically bound ('allPurpose') materials. */
  pxr::UsdShadeMaterial mtl = api.ComputeBoundMaterial();

  /* If no generic material could be resolved, also check for 'preview' and
   * 'full' purpose materials as fallbacks. */
  if (!mtl) {
    mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->preview);
  }

  if (!mtl) {
    mtl = api.ComputeBoundMaterial(pxr::UsdShadeTokens->full);
  }

  return mtl;
}

static void assign_materials(Main *bmain,
                             Object *ob,
                             const std::map<pxr::SdfPath, int> &mat_index_map,
                             const USDImportParams &params,
                             pxr::UsdStageRefPtr stage,
                             std::map<std::string, Material *> &mat_name_to_mat,
                             std::map<std::string, std::string> &usd_path_to_mat_name)
{
  if (!(stage && bmain && ob)) {
    return;
  }

  if (mat_index_map.size() > MAXMAT) {
    return;
  }

  blender::io::usd::USDMaterialReader mat_reader(params, bmain);

  for (std::map<pxr::SdfPath, int>::const_iterator it = mat_index_map.begin();
       it != mat_index_map.end();
       ++it) {

    Material *assigned_mat = blender::io::usd::find_existing_material(
        it->first, params, mat_name_to_mat, usd_path_to_mat_name);
    if (!assigned_mat) {
      /* Blender material doesn't exist, so create it now. */

      /* Look up the USD material. */
      pxr::UsdPrim prim = stage->GetPrimAtPath(it->first);
      pxr::UsdShadeMaterial usd_mat(prim);

      if (!usd_mat) {
        std::cout << "WARNING: Couldn't construct USD material from prim " << it->first
                  << std::endl;
        continue;
      }

      /* Add the Blender material. */
      assigned_mat = mat_reader.add_material(usd_mat);

      if (!assigned_mat) {
        std::cout << "WARNING: Couldn't create Blender material from USD material " << it->first
                  << std::endl;
        continue;
      }

      const std::string mat_name = pxr::TfMakeValidIdentifier(assigned_mat->id.name + 2);
      mat_name_to_mat[mat_name] = assigned_mat;

      if (params.mtl_name_collision_mode == USD_MTL_NAME_COLLISION_MAKE_UNIQUE) {
        /* Record the name of the Blender material we created for the USD material
         * with the given path. */
        usd_path_to_mat_name[it->first.GetAsString()] = mat_name;
      }
    }

    if (assigned_mat) {
      BKE_object_material_assign_single_obdata(bmain, ob, assigned_mat, it->second);
    }
    else {
      /* This shouldn't happen. */
      std::cout << "WARNING: Couldn't assign material " << it->first << std::endl;
    }
  }
  if (ob->totcol > 0) {
    ob->actcol = 1;
  }
}

}  // namespace utils

static void *add_customdata_cb(Mesh *mesh, const char *name, const int data_type)
{
  eCustomDataType cd_data_type = static_cast<eCustomDataType>(data_type);
  void *cd_ptr;
  CustomData *loopdata;
  int numloops;

  /* unsupported custom data type -- don't do anything. */
  if (!ELEM(cd_data_type, CD_PROP_FLOAT2, CD_PROP_BYTE_COLOR)) {
    return nullptr;
  }

  loopdata = &mesh->ldata;
  cd_ptr = CustomData_get_layer_named_for_write(loopdata, cd_data_type, name, mesh->totloop);
  if (cd_ptr != nullptr) {
    /* layer already exists, so just return it. */
    return cd_ptr;
  }

  /* Create a new layer. */
  numloops = mesh->totloop;
  cd_ptr = CustomData_add_layer_named(loopdata, cd_data_type, CD_SET_DEFAULT, numloops, name);
  return cd_ptr;
}

namespace blender::io::usd {

USDMeshReader::USDMeshReader(const pxr::UsdPrim &prim,
                             const USDImportParams &import_params,
                             const ImportSettings &settings)
    : USDGeomReader(prim, import_params, settings),
      mesh_prim_(prim),
      is_left_handed_(false),
      has_uvs_(false),
      is_time_varying_(false),
      is_initial_load_(false)
{
}

void USDMeshReader::create_object(Main *bmain, const double /* motionSampleTime */)
{
  Mesh *mesh = BKE_mesh_add(bmain, name_.c_str());

  object_ = BKE_object_add_only_object(bmain, OB_MESH, name_.c_str());
  object_->data = mesh;
}

void USDMeshReader::read_object_data(Main *bmain, const double motionSampleTime)
{
  Mesh *mesh = (Mesh *)object_->data;

  is_initial_load_ = true;
  const USDMeshReadParams params = create_mesh_read_params(motionSampleTime,
                                                           import_params_.mesh_read_flag);

  Mesh *read_mesh = this->read_mesh(mesh, params, nullptr);

  is_initial_load_ = false;
  if (read_mesh != mesh) {
    BKE_mesh_nomain_to_mesh(read_mesh, mesh, object_);
  }

  readFaceSetsSample(bmain, mesh, motionSampleTime);

  if (mesh_prim_.GetPointsAttr().ValueMightBeTimeVarying()) {
    is_time_varying_ = true;
  }

  if (is_time_varying_) {
    add_cache_modifier();
  }

  if (import_params_.import_subdiv) {
    pxr::TfToken subdivScheme;
    mesh_prim_.GetSubdivisionSchemeAttr().Get(&subdivScheme, motionSampleTime);

    if (subdivScheme == pxr::UsdGeomTokens->catmullClark) {
      add_subdiv_modifier();
    }
  }

  USDXformReader::read_object_data(bmain, motionSampleTime);
}  // namespace blender::io::usd

bool USDMeshReader::valid() const
{
  return bool(mesh_prim_);
}

bool USDMeshReader::topology_changed(const Mesh *existing_mesh, const double motionSampleTime)
{
  /* TODO(makowalski): Is it the best strategy to cache the mesh
   * geometry in this function?  This needs to be revisited. */

  mesh_prim_.GetFaceVertexIndicesAttr().Get(&face_indices_, motionSampleTime);
  mesh_prim_.GetFaceVertexCountsAttr().Get(&face_counts_, motionSampleTime);
  mesh_prim_.GetPointsAttr().Get(&positions_, motionSampleTime);

  pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh_prim_);

  /* TODO(makowalski): Reading normals probably doesn't belong in this function,
   * as this is not required to determine if the topology has changed. */

  /* If 'normals' and 'primvars:normals' are both specified, the latter has precedence. */
  pxr::UsdGeomPrimvar primvar = primvarsAPI.GetPrimvar(usdtokens::normalsPrimvar);
  if (primvar.HasValue()) {
    primvar.ComputeFlattened(&normals_, motionSampleTime);
    normal_interpolation_ = primvar.GetInterpolation();
  }
  else {
    mesh_prim_.GetNormalsAttr().Get(&normals_, motionSampleTime);
    normal_interpolation_ = mesh_prim_.GetNormalsInterpolation();
  }

  return positions_.size() != existing_mesh->totvert ||
         face_counts_.size() != existing_mesh->totpoly ||
         face_indices_.size() != existing_mesh->totloop;
}

void USDMeshReader::read_mpolys(Mesh *mesh)
{
  MutableSpan<int> poly_offsets = mesh->poly_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();

  int loop_index = 0;

  for (int i = 0; i < face_counts_.size(); i++) {
    const int face_size = face_counts_[i];

    poly_offsets[i] = loop_index;

    /* Polygons are always assumed to be smooth-shaded. If the mesh should be flat-shaded,
     * this is encoded in custom loop normals. */

    if (is_left_handed_) {
      int loop_end_index = loop_index + (face_size - 1);
      for (int f = 0; f < face_size; ++f, ++loop_index) {
        corner_verts[loop_index] = face_indices_[loop_end_index - f];
      }
    }
    else {
      for (int f = 0; f < face_size; ++f, ++loop_index) {
        corner_verts[loop_index] = face_indices_[loop_index];
      }
    }
  }

  BKE_mesh_calc_edges(mesh, false, false);
}

void USDMeshReader::read_uvs(Mesh *mesh, const double motionSampleTime, const bool load_uvs)
{
  uint loop_index = 0;
  uint rev_loop_index = 0;
  uint uv_index = 0;

  const CustomData *ldata = &mesh->ldata;

  struct UVSample {
    pxr::VtVec2fArray uvs;
    pxr::TfToken interpolation;
  };

  std::vector<UVSample> uv_primvars(ldata->totlayer);

  pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh_prim_);

  if (has_uvs_) {
    for (int layer_idx = 0; layer_idx < ldata->totlayer; layer_idx++) {
      const CustomDataLayer *layer = &ldata->layers[layer_idx];
      std::string layer_name = std::string(layer->name);
      if (layer->type != CD_PROP_FLOAT2) {
        continue;
      }

      pxr::TfToken uv_token;

      /* If first time seeing uv token, store in map of `<layer->uid, TfToken>`. */
      if (uv_token_map_.find(layer_name) == uv_token_map_.end()) {
        uv_token = pxr::TfToken(layer_name);
        uv_token_map_.insert(std::make_pair(layer_name, uv_token));
      }
      else {
        uv_token = uv_token_map_.at(layer_name);
      }

      /* Early out if no token found, this should never happen */
      if (uv_token.IsEmpty()) {
        continue;
      }
      /* Early out if not first load and UVs aren't animated. */
      if (!load_uvs && primvar_varying_map_.find(uv_token) != primvar_varying_map_.end() &&
          !primvar_varying_map_.at(uv_token)) {
        continue;
      }

      /* Early out if mesh doesn't have primvar. */
      if (!primvarsAPI.HasPrimvar(uv_token)) {
        continue;
      }

      if (pxr::UsdGeomPrimvar uv_primvar = primvarsAPI.GetPrimvar(uv_token)) {
        uv_primvar.ComputeFlattened(&uv_primvars[layer_idx].uvs, motionSampleTime);
        uv_primvars[layer_idx].interpolation = uv_primvar.GetInterpolation();
      }
    }
  }

  const Span<int> corner_verts = mesh->corner_verts();
  for (int i = 0; i < face_counts_.size(); i++) {
    const int face_size = face_counts_[i];

    rev_loop_index = loop_index + (face_size - 1);

    for (int f = 0; f < face_size; f++, loop_index++, rev_loop_index--) {

      for (int layer_idx = 0; layer_idx < ldata->totlayer; layer_idx++) {
        const CustomDataLayer *layer = &ldata->layers[layer_idx];
        if (layer->type != CD_PROP_FLOAT2) {
          continue;
        }

        /* Early out if mismatched layer sizes. */
        if (layer_idx > uv_primvars.size()) {
          continue;
        }

        /* Early out if no uvs loaded. */
        if (uv_primvars[layer_idx].uvs.empty()) {
          continue;
        }

        const UVSample &sample = uv_primvars[layer_idx];

        if (!ELEM(sample.interpolation,
                  pxr::UsdGeomTokens->faceVarying,
                  pxr::UsdGeomTokens->vertex)) {
          std::cerr << "WARNING: unexpected interpolation type " << sample.interpolation
                    << " for uv " << layer->name << std::endl;
          continue;
        }

        /* For Vertex interpolation, use the vertex index. */
        int usd_uv_index = sample.interpolation == pxr::UsdGeomTokens->vertex ?
                               corner_verts[loop_index] :
                               loop_index;

        if (usd_uv_index >= sample.uvs.size()) {
          std::cerr << "WARNING: out of bounds uv index " << usd_uv_index << " for uv "
                    << layer->name << " of size " << sample.uvs.size() << std::endl;
          continue;
        }

        float2 *mloopuv = static_cast<float2 *>(layer->data);
        if (is_left_handed_) {
          uv_index = rev_loop_index;
        }
        else {
          uv_index = loop_index;
        }
        mloopuv[uv_index][0] = sample.uvs[usd_uv_index][0];
        mloopuv[uv_index][1] = sample.uvs[usd_uv_index][1];
      }
    }
  }
}

void USDMeshReader::read_colors(Mesh *mesh, const double motionSampleTime)
{
  if (!(mesh && mesh_prim_ && mesh->totloop > 0)) {
    return;
  }

  /* Early out if we read the display color before and if this attribute isn't animated. */
  if (primvar_varying_map_.find(usdtokens::displayColor) != primvar_varying_map_.end() &&
      !primvar_varying_map_.at(usdtokens::displayColor)) {
    return;
  }

  pxr::UsdGeomPrimvar color_primvar = mesh_prim_.GetDisplayColorPrimvar();

  if (!color_primvar.HasValue()) {
    return;
  }

  pxr::TfToken interp = color_primvar.GetInterpolation();

  if (interp == pxr::UsdGeomTokens->varying) {
    std::cerr << "WARNING: Unsupported varying interpolation for display colors\n" << std::endl;
    return;
  }

  if (primvar_varying_map_.find(usdtokens::displayColor) == primvar_varying_map_.end()) {
    bool might_be_time_varying = color_primvar.ValueMightBeTimeVarying();
    primvar_varying_map_.insert(std::make_pair(usdtokens::displayColor, might_be_time_varying));
    if (might_be_time_varying) {
      is_time_varying_ = true;
    }
  }

  pxr::VtArray<pxr::GfVec3f> display_colors;

  if (!color_primvar.ComputeFlattened(&display_colors, motionSampleTime)) {
    std::cerr << "WARNING: Couldn't compute display colors\n" << std::endl;
    return;
  }

  if ((interp == pxr::UsdGeomTokens->faceVarying && display_colors.size() != mesh->totloop) ||
      (interp == pxr::UsdGeomTokens->vertex && display_colors.size() != mesh->totvert) ||
      (interp == pxr::UsdGeomTokens->constant && display_colors.size() != 1) ||
      (interp == pxr::UsdGeomTokens->uniform && display_colors.size() != mesh->totpoly)) {
    std::cerr << "WARNING: display colors count mismatch\n" << std::endl;
    return;
  }

  void *cd_ptr = add_customdata_cb(mesh, "displayColor", CD_PROP_BYTE_COLOR);

  if (!cd_ptr) {
    std::cerr << "WARNING: Couldn't add displayColor custom data.\n";
    return;
  }

  MLoopCol *colors = static_cast<MLoopCol *>(cd_ptr);

  const OffsetIndices polys = mesh->polys();
  const Span<int> corner_verts = mesh->corner_verts();
  for (const int i : polys.index_range()) {
    const IndexRange poly = polys[i];
    for (int j = 0; j < poly.size(); ++j) {
      int loop_index = poly[j];

      /* Default for constant varying interpolation. */
      int usd_index = 0;

      if (interp == pxr::UsdGeomTokens->vertex) {
        usd_index = corner_verts[loop_index];
      }
      else if (interp == pxr::UsdGeomTokens->faceVarying) {
        usd_index = poly.start();
        if (is_left_handed_) {
          usd_index += poly.size() - 1 - j;
        }
        else {
          usd_index += j;
        }
      }
      else if (interp == pxr::UsdGeomTokens->uniform) {
        /* Uniform varying uses the poly index. */
        usd_index = i;
      }

      if (usd_index >= display_colors.size()) {
        continue;
      }

      colors[loop_index].r = unit_float_to_uchar_clamp(display_colors[usd_index][0]);
      colors[loop_index].g = unit_float_to_uchar_clamp(display_colors[usd_index][1]);
      colors[loop_index].b = unit_float_to_uchar_clamp(display_colors[usd_index][2]);
      colors[loop_index].a = unit_float_to_uchar_clamp(1.0);
    }
  }

  BKE_id_attributes_active_color_set(&mesh->id, "displayColor");
}

void USDMeshReader::read_vertex_creases(Mesh *mesh, const double motionSampleTime)
{
  pxr::VtIntArray corner_indices;
  if (!mesh_prim_.GetCornerIndicesAttr().Get(&corner_indices, motionSampleTime)) {
    return;
  }

  pxr::VtIntArray corner_sharpnesses;
  if (!mesh_prim_.GetCornerSharpnessesAttr().Get(&corner_sharpnesses, motionSampleTime)) {
    return;
  }

  /* It is fine to have fewer indices than vertices, but never the other way other. */
  if (corner_indices.size() > mesh->totvert) {
    std::cerr << "WARNING: too many vertex crease for mesh " << prim_path_ << std::endl;
    return;
  }

  if (corner_indices.size() != corner_sharpnesses.size()) {
    std::cerr << "WARNING: vertex crease indices and sharpnesses count mismatch for mesh "
              << prim_path_ << std::endl;
    return;
  }

  float *creases = static_cast<float *>(
      CustomData_add_layer(&mesh->vdata, CD_CREASE, CD_SET_DEFAULT, mesh->totvert));

  for (size_t i = 0; i < corner_indices.size(); i++) {
    creases[corner_indices[i]] = corner_sharpnesses[i];
  }
}

void USDMeshReader::process_normals_vertex_varying(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  if (normals_.empty()) {
    return;
  }

  if (normals_.size() != mesh->totvert) {
    std::cerr << "WARNING: vertex varying normals count mismatch for mesh " << prim_path_
              << std::endl;
    return;
  }

  MutableSpan vert_normals{(float3 *)BKE_mesh_vert_normals_for_write(mesh), mesh->totvert};
  BLI_STATIC_ASSERT(sizeof(normals_[0]) == sizeof(float3), "Expected float3 normals size");
  vert_normals.copy_from({(float3 *)normals_.data(), int64_t(normals_.size())});
  BKE_mesh_vert_normals_clear_dirty(mesh);
}

void USDMeshReader::process_normals_face_varying(Mesh *mesh)
{
  if (normals_.empty()) {
    return;
  }

  /* Check for normals count mismatches to prevent crashes. */
  if (normals_.size() != mesh->totloop) {
    std::cerr << "WARNING: loop normal count mismatch for mesh " << mesh->id.name << std::endl;
    return;
  }

  mesh->flag |= ME_AUTOSMOOTH;

  long int loop_count = normals_.size();

  float(*lnors)[3] = static_cast<float(*)[3]>(
      MEM_malloc_arrayN(loop_count, sizeof(float[3]), "USD::FaceNormals"));

  const OffsetIndices polys = mesh->polys();
  for (const int i : polys.index_range()) {
    const IndexRange poly = polys[i];
    for (int j = 0; j < poly.size(); j++) {
      int blender_index = poly.start() + j;

      int usd_index = poly.start();
      if (is_left_handed_) {
        usd_index += poly.size() - 1 - j;
      }
      else {
        usd_index += j;
      }

      lnors[blender_index][0] = normals_[usd_index][0];
      lnors[blender_index][1] = normals_[usd_index][1];
      lnors[blender_index][2] = normals_[usd_index][2];
    }
  }
  BKE_mesh_set_custom_normals(mesh, lnors);

  MEM_freeN(lnors);
}

void USDMeshReader::process_normals_uniform(Mesh *mesh)
{
  if (normals_.empty()) {
    return;
  }

  /* Check for normals count mismatches to prevent crashes. */
  if (normals_.size() != mesh->totpoly) {
    std::cerr << "WARNING: uniform normal count mismatch for mesh " << mesh->id.name << std::endl;
    return;
  }

  float(*lnors)[3] = static_cast<float(*)[3]>(
      MEM_malloc_arrayN(mesh->totloop, sizeof(float[3]), "USD::FaceNormals"));

  const OffsetIndices polys = mesh->polys();
  for (const int i : polys.index_range()) {
    for (const int corner : polys[i]) {
      lnors[corner][0] = normals_[i][0];
      lnors[corner][1] = normals_[i][1];
      lnors[corner][2] = normals_[i][2];
    }
  }

  mesh->flag |= ME_AUTOSMOOTH;
  BKE_mesh_set_custom_normals(mesh, lnors);

  MEM_freeN(lnors);
}

void USDMeshReader::read_mesh_sample(ImportSettings *settings,
                                     Mesh *mesh,
                                     const double motionSampleTime,
                                     const bool new_mesh)
{
  /* Note that for new meshes we always want to read verts and polys,
   * regardless of the value of the read_flag, to avoid a crash downstream
   * in code that expect this data to be there. */

  if (new_mesh || (settings->read_flag & MOD_MESHSEQ_READ_VERT) != 0) {
    MutableSpan<float3> vert_positions = mesh->vert_positions_for_write();
    for (int i = 0; i < positions_.size(); i++) {
      vert_positions[i] = {positions_[i][0], positions_[i][1], positions_[i][2]};
    }
    BKE_mesh_tag_positions_changed(mesh);

    read_vertex_creases(mesh, motionSampleTime);
  }

  if (new_mesh || (settings->read_flag & MOD_MESHSEQ_READ_POLY) != 0) {
    read_mpolys(mesh);
    if (normal_interpolation_ == pxr::UsdGeomTokens->faceVarying) {
      process_normals_face_varying(mesh);
    }
    else if (normal_interpolation_ == pxr::UsdGeomTokens->uniform) {
      process_normals_uniform(mesh);
    }
  }

  /* Process point normals after reading polys. */
  if ((settings->read_flag & MOD_MESHSEQ_READ_VERT) != 0 &&
      normal_interpolation_ == pxr::UsdGeomTokens->vertex) {
    process_normals_vertex_varying(mesh);
  }

  if ((settings->read_flag & MOD_MESHSEQ_READ_UV) != 0) {
    read_uvs(mesh, motionSampleTime, new_mesh);
  }

  if ((settings->read_flag & MOD_MESHSEQ_READ_COLOR) != 0) {
    read_colors(mesh, motionSampleTime);
  }
}

void USDMeshReader::assign_facesets_to_material_indices(double motionSampleTime,
                                                        MutableSpan<int> material_indices,
                                                        std::map<pxr::SdfPath, int> *r_mat_map)
{
  if (r_mat_map == nullptr) {
    return;
  }

  /* Find the geom subsets that have bound materials.
   * We don't call #pxr::UsdShadeMaterialBindingAPI::GetMaterialBindSubsets()
   * because this function returns only those subsets that are in the 'materialBind'
   * family, but, in practice, applications (like Houdini) might export subsets
   * in different families that are bound to materials.
   * TODO(makowalski): Reassess if the above is the best approach. */
  const std::vector<pxr::UsdGeomSubset> subsets = pxr::UsdGeomSubset::GetAllGeomSubsets(
      mesh_prim_);

  int current_mat = 0;
  if (!subsets.empty()) {
    for (const pxr::UsdGeomSubset &subset : subsets) {

      pxr::UsdShadeMaterial subset_mtl = utils::compute_bound_material(subset.GetPrim());
      if (!subset_mtl) {
        continue;
      }

      pxr::SdfPath subset_mtl_path = subset_mtl.GetPath();

      if (subset_mtl_path.IsEmpty()) {
        continue;
      }

      if (r_mat_map->find(subset_mtl_path) == r_mat_map->end()) {
        (*r_mat_map)[subset_mtl_path] = 1 + current_mat++;
      }

      const int mat_idx = (*r_mat_map)[subset_mtl_path] - 1;

      pxr::UsdAttribute indicesAttribute = subset.GetIndicesAttr();
      pxr::VtIntArray indices;
      indicesAttribute.Get(&indices, motionSampleTime);

      for (const int i : indices) {
        material_indices[i] = mat_idx;
      }
    }
  }

  if (r_mat_map->empty()) {

    pxr::UsdShadeMaterial mtl = utils::compute_bound_material(prim_);
    if (mtl) {
      pxr::SdfPath mtl_path = mtl.GetPath();

      if (!mtl_path.IsEmpty()) {
        r_mat_map->insert(std::make_pair(mtl.GetPath(), 1));
      }
    }
  }
}

void USDMeshReader::readFaceSetsSample(Main *bmain, Mesh *mesh, const double motionSampleTime)
{
  if (!import_params_.import_materials) {
    return;
  }

  std::map<pxr::SdfPath, int> mat_map;

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<int> material_indices = attributes.lookup_or_add_for_write_span<int>(
      "material_index", ATTR_DOMAIN_FACE);
  this->assign_facesets_to_material_indices(motionSampleTime, material_indices.span, &mat_map);
  material_indices.finish();
  /* Build material name map if it's not built yet. */
  if (this->settings_->mat_name_to_mat.empty()) {
    build_material_map(bmain, &this->settings_->mat_name_to_mat);
  }
  utils::assign_materials(bmain,
                          object_,
                          mat_map,
                          this->import_params_,
                          this->prim_.GetStage(),
                          this->settings_->mat_name_to_mat,
                          this->settings_->usd_path_to_mat_name);
}

Mesh *USDMeshReader::read_mesh(Mesh *existing_mesh,
                               const USDMeshReadParams params,
                               const char ** /* err_str */)
{
  if (!mesh_prim_) {
    return existing_mesh;
  }

  mesh_prim_.GetOrientationAttr().Get(&orientation_);
  if (orientation_ == pxr::UsdGeomTokens->leftHanded) {
    is_left_handed_ = true;
  }

  pxr::UsdGeomPrimvarsAPI primvarsAPI(mesh_prim_);

  std::vector<pxr::TfToken> uv_tokens;

  /* Currently we only handle UV primvars. */
  if (params.read_flags & MOD_MESHSEQ_READ_UV) {

    std::vector<pxr::UsdGeomPrimvar> primvars = primvarsAPI.GetPrimvars();

    for (pxr::UsdGeomPrimvar p : primvars) {

      pxr::TfToken name = p.GetPrimvarName();
      pxr::SdfValueTypeName type = p.GetTypeName();

      bool is_uv = false;

      /* Assume all UVs are stored in one of these primvar types */
      if (ELEM(type,
               pxr::SdfValueTypeNames->TexCoord2hArray,
               pxr::SdfValueTypeNames->TexCoord2fArray,
               pxr::SdfValueTypeNames->TexCoord2dArray)) {
        is_uv = true;
      }
      /* In some cases, the st primvar is stored as float2 values. */
      else if (name == usdtokens::st && type == pxr::SdfValueTypeNames->Float2Array) {
        is_uv = true;
      }

      if (is_uv) {

        pxr::TfToken interp = p.GetInterpolation();

        if (!ELEM(interp, pxr::UsdGeomTokens->faceVarying, pxr::UsdGeomTokens->vertex)) {
          continue;
        }

        uv_tokens.push_back(p.GetBaseName());
        has_uvs_ = true;

        /* Record whether the UVs might be time varying. */
        if (primvar_varying_map_.find(name) == primvar_varying_map_.end()) {
          bool might_be_time_varying = p.ValueMightBeTimeVarying();
          primvar_varying_map_.insert(std::make_pair(name, might_be_time_varying));
          if (might_be_time_varying) {
            is_time_varying_ = true;
          }
        }
      }
    }
  }

  Mesh *active_mesh = existing_mesh;
  bool new_mesh = false;

  /* TODO(makowalski): implement the optimization of only updating the mesh points when
   * the topology is consistent, as in the Alembic importer. */

  ImportSettings settings;
  settings.read_flag |= params.read_flags;

  if (topology_changed(existing_mesh, params.motion_sample_time)) {
    new_mesh = true;
    active_mesh = BKE_mesh_new_nomain_from_template(
        existing_mesh, positions_.size(), 0, face_indices_.size(), face_counts_.size());

    for (pxr::TfToken token : uv_tokens) {
      add_customdata_cb(active_mesh, token.GetText(), CD_PROP_FLOAT2);
    }
  }

  read_mesh_sample(
      &settings, active_mesh, params.motion_sample_time, new_mesh || is_initial_load_);

  if (new_mesh) {
    /* Here we assume that the number of materials doesn't change, i.e. that
     * the material slots that were created when the object was loaded from
     * USD are still valid now. */
    if (active_mesh->totpoly != 0 && import_params_.import_materials) {
      std::map<pxr::SdfPath, int> mat_map;
      bke::MutableAttributeAccessor attributes = active_mesh->attributes_for_write();
      bke::SpanAttributeWriter<int> material_indices =
          attributes.lookup_or_add_for_write_span<int>("material_index", ATTR_DOMAIN_FACE);
      assign_facesets_to_material_indices(
          params.motion_sample_time, material_indices.span, &mat_map);
      material_indices.finish();
    }
  }

  return active_mesh;
}

}  // namespace blender::io::usd
