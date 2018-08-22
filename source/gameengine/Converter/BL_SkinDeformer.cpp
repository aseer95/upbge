/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Converter/BL_SkinDeformer.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

// Eigen3 stuff used for BGEDeformVerts
#include <Eigen/Core>
#include <Eigen/LU>

#include "BL_SkinDeformer.h"
#include <string>
#include "RAS_IPolygonMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_BoundingBox.h"

#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "BLI_utildefines.h"
#include "BKE_armature.h"
#include "BKE_action.h"

extern "C" {
	#include "BKE_lattice.h"
	#include "BKE_deform.h"
}

#include "GPU_material.h"
#include "GPU_extensions.h"
#include "GPU_shader.h"

#include <GL/glew.h>

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_math.h"

static short get_deformflags(Object *bmeshobj)
{
	short flags = ARM_DEF_VGROUP;

	ModifierData *md;
	for (md = (ModifierData *)bmeshobj->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Armature) {
			flags |= ((ArmatureModifierData *)md)->deformflag;
			break;
		}
	}

	return flags;
}

BL_SkinDeformer::BL_SkinDeformer(KX_GameObject *gameobj,
                                 Object *bmeshobj_old, // Blender object that owns the new mesh
                                 Object *bmeshobj_new, // Blender object that owns the original mesh
                                 RAS_Mesh *mesh,
                                 BL_ArmatureObject *arma)
	:BL_MeshDeformer(gameobj, bmeshobj_old, mesh),
	m_armobj(arma),
	m_lastArmaUpdate(-1),
	m_copyNormals(false)
{
	// this is needed to ensure correct deformation of mesh:
	// the deformation is done with Blender's armature_deform_verts() function
	// that takes an object as parameter and not a mesh. The object matrice is used
	// in the calculation, so we must use the matrix of the original object to
	// simulate a pure replacement of the mesh.
	copy_m4_m4(m_obmat, bmeshobj_new->obmat);
	m_deformflags = get_deformflags(bmeshobj_new);

	const unsigned int defbase_tot = BLI_listbase_count(&m_objMesh->defbase);

	m_dfnrToPC.resize(defbase_tot);
	Object *par_arma = m_armobj->GetArmatureObject();

	int i;
	bDeformGroup *dg;
	for (i = 0, dg = (bDeformGroup *)m_objMesh->defbase.first; dg; ++i, dg = dg->next) {
		m_dfnrToPC[i] = BKE_pose_channel_find_name(par_arma->pose, dg->name);

		if (m_dfnrToPC[i] && m_dfnrToPC[i]->bone->flag & BONE_NO_DEFORM) {
			m_dfnrToPC[i] = nullptr;
		}
	}
}

BL_SkinDeformer::~BL_SkinDeformer()
{
}

void BL_SkinDeformer::Initialize()
{
	BL_MeshDeformer::Initialize();

	VerifyHardwareSkinning();
}

void BL_SkinDeformer::VerifyHardwareSkinning()
{
	if (m_armobj->GetVertDeformType() != ARM_VDEF_BGE_GPU) {
		return;
	}

	// Check to see if we can do this skinning in hardware, and fallback to software if we cannot.
	// Each matrix is sixteen components, and we don't want to use more than half of the available components (or maybe so).
	const unsigned int defbase_tot = m_dfnrToPC.size();
	const unsigned int max = clamp_i(GPU_max_vertex_uniform_components() / 16, 0, 128);

	if (defbase_tot > max) {
		printf("Skinned mesh with %u bones not supported by hardware (max: %d): %s. Using software skinning.\n",
		       defbase_tot, max, m_objMesh->id.name);
		m_armobj->SetVertDeformType(ARM_VDEF_BGE_CPU);
	}
	else {
		m_poseMatrices.resize(defbase_tot);

		const unsigned short size = m_slots.size();
		m_skinVertData.resize(size);
		for (unsigned short i = 0; i < size; ++i) {
			m_skinVertData[i].resize(m_slots[i].m_displayArray->GetVertexCount());
		}
	}
}

void BL_SkinDeformer::Apply(RAS_DisplayArray *array)
{
	for (DisplayArraySlot& slot : m_slots) {
		if (slot.m_displayArray == array) { // TODO same here
			const short modifiedFlag = slot.m_arrayUpdateClient.GetInvalidAndClear();
			if (modifiedFlag != RAS_DisplayArray::NONE_MODIFIED) {
				/// Update vertex data from the original mesh.
				array->UpdateFrom(slot.m_origDisplayArray, modifiedFlag);
			}

			break;
		}
	}
}

bool BL_SkinDeformer::UseShaderSkinning() const
{
	return (m_armobj->GetVertDeformType() == ARM_VDEF_BGE_GPU);
}

RAS_Deformer::SkinShaderData BL_SkinDeformer::GetSkinningShaderData(RAS_DisplayArray *array) const
{
	for (unsigned short i = 0, size = m_slots.size(); i < size; ++i) { // TODO une classes de donnée de deformation par array: RAS_DeformerArrayData stocké dans le DAB.
		if (m_slots[i].m_displayArray == array) {
			return {m_skinVertData[i].data(), reinterpret_cast<const float *>(m_poseMatrices.data()), (unsigned char)m_poseMatrices.size()};
		}
	}

	BLI_assert(false);
	return {nullptr, nullptr, 0};
}

void BL_SkinDeformer::BlenderDeformVerts()
{
	float obmat[4][4];  // the original object matrix
	Object *par_arma = m_armobj->GetArmatureObject();

	// save matrix first
	copy_m4_m4(obmat, m_objMesh->obmat);
	// set reference matrix
	copy_m4_m4(m_objMesh->obmat, m_obmat);

	armature_deform_verts(par_arma, m_objMesh, nullptr, (float(*)[3])m_transverts.data(), nullptr, m_bmesh->totvert, m_deformflags, nullptr, nullptr);

	// restore matrix
	copy_m4_m4(m_objMesh->obmat, obmat);

	RecalcNormals();
}

void BL_SkinDeformer::DeformVerts()
{
	MDeformVert *dverts = m_bmesh->dvert;
	Eigen::Matrix4f pre_mat, post_mat, chan_mat, norm_chan_mat;

	if (!dverts) {
		return;
	}

	const unsigned short defbase_tot = m_dfnrToPC.size();

	post_mat = Eigen::Matrix4f::Map((float *)m_obmat).inverse() * Eigen::Matrix4f::Map((float *)m_armobj->GetArmatureObject()->obmat);
	pre_mat = post_mat.inverse();

	MDeformVert *dv = dverts;
	MDeformWeight *dw;

	for (int i = 0; i < m_bmesh->totvert; ++i, dv++) {
		float contrib = 0.0f, weight, max_weight = -1.0f;
		bPoseChannel *pchan = nullptr;
		Eigen::Vector3f normorg(m_bmesh->mvert[i].no[0], m_bmesh->mvert[i].no[1], m_bmesh->mvert[i].no[2]);
		Eigen::Map<Eigen::Vector3f> norm = Eigen::Vector3f::Map(m_transnors[i].data);
		Eigen::Vector4f vec(0.0f, 0.0f, 0.0f, 1.0f);
		Eigen::Vector4f co(m_transverts[i].x,
		                   m_transverts[i].y,
		                   m_transverts[i].z,
		                   1.0f);

		if (!dv->totweight) {
			continue;
		}

		co = pre_mat * co;

		dw = dv->dw;

		for (unsigned int j = dv->totweight; j != 0; j--, dw++) {
			const int index = dw->def_nr;

			if (index < defbase_tot && (pchan = m_dfnrToPC[index])) {
				weight = dw->weight;

				if (weight) {
					chan_mat = Eigen::Matrix4f::Map((float *)pchan->chan_mat);

					// Update Vertex Position
					vec.noalias() += (chan_mat * co - co) * weight;

					// Save the most influential channel so we can use it to update the vertex normal
					if (weight > max_weight) {
						max_weight = weight;
						norm_chan_mat = chan_mat;
					}

					contrib += weight;
				}
			}
		}

		// Update Vertex Normal
		norm = norm_chan_mat.topLeftCorner<3, 3>() * normorg;

		co.noalias() += vec / contrib;
		co[3] = 1.0f; // Make sure we have a 1 for the w component!

		co = post_mat * co;

		m_transverts[i] = mt::vec3(co[0], co[1], co[2]);
	}
	m_copyNormals = true;
}

void BL_SkinDeformer::GPUDeformVerts()
{
	for (unsigned short i = 0, size = m_dfnrToPC.size(); i < size; ++i) {
		if (m_dfnrToPC[i]) {
			m_poseMatrices[i] = mt::mat4(m_dfnrToPC[i]->chan_mat);
		}
	}

	for (unsigned short k = 0, size = m_slots.size(); k < size; ++k) {
		std::vector<SkinVertData>& skinverts = m_skinVertData[k];
		RAS_DisplayArray *array = m_slots[k].m_displayArray;

		for (unsigned int i = 0, size = skinverts.size(); i < size; ++i) {
			const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);
			MDeformVert *dv = &m_bmesh->dvert[vinfo.GetOrigIndex()];
			SkinVertData& data = skinverts[i];

			if (dv->totweight <= 4) {
					// We have no more than four weights, just copy them over.
					data.numbones = dv->totweight;

					MDeformWeight *dw = dv->dw;
					for (unsigned short j = 0; j < dv->totweight && j < 4; ++j, ++dw) {
						data.indices[j] = dw->def_nr;
						data.weights[j] = dw->weight;
					}
			}
			else {
				// We have more than four weights, pick the four most influential bones
				unsigned short j;
				for (j = 0; j < 4; ++j) {
					float maxval = -1.0f;
					unsigned char maxidx;

					MDeformWeight *dw = dv->dw;
					for (unsigned short k = 0; k < dv->totweight; ++k, ++dw) {
						if (dw->weight > maxval && !ELEM(dw->def_nr, data.indices[0], data.indices[1], data.indices[2], data.indices[3])) {
							maxval = dw->weight;
							maxidx = dw->def_nr;
						}
					}

					if (maxval <= 0.0f) {
						break;
					}

					data.indices[j] = maxidx;
					data.weights[j] = maxval;

					dw = dv->dw;
				}

				data.numbones = j;

				normalize_vn(data.weights, 4);
			}
		}
	}
}

void BL_SkinDeformer::UpdateTransverts()
{
	if (m_transverts.empty()) {
		return;
	}

	// AABB Box : min/max.
	mt::vec3 aabbMin(FLT_MAX);
	mt::vec3 aabbMax(-FLT_MAX);

	if (m_bDynamic) {
		const bool autoUpdate = m_gameobj->GetAutoUpdateBounds();

		// the vertex cache is unique to this deformer, no need to update it
		// if it wasn't updated! We must update all the materials at once
		// because we will not get here again for the other material
		for (const DisplayArraySlot& slot : m_slots) {
			RAS_DisplayArray *array = slot.m_displayArray;
			// for each vertex
			// copy the untransformed data from the original mvert
			for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
				const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);
				const mt::vec3 pos = mt::vec3(m_transverts[vinfo.GetOrigIndex()]);
				array->SetPosition(i, pos);

				if (autoUpdate) {
					aabbMin = mt::vec3::Min(aabbMin, pos);
					aabbMax = mt::vec3::Max(aabbMax, pos);
				}
			}
			if (m_copyNormals) {
				for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
					const RAS_VertexInfo& vinfo = array->GetVertexInfo(i);
					array->SetNormal(i, m_transnors[vinfo.GetOrigIndex()]);
				}
			}

			array->NotifyUpdate(RAS_DisplayArray::POSITION_MODIFIED | RAS_DisplayArray::NORMAL_MODIFIED);
		}

		m_boundingBox->SetAabb(aabbMin, aabbMax);

		if (m_copyNormals) {
			m_copyNormals = false;
		}
	}
}

bool BL_SkinDeformer::UpdateInternal(bool shape_applied)
{
	/* See if the armature has been updated for this frame */
	if (PoseUpdated()) {
		m_armobj->ApplyPose();

		switch (m_armobj->GetVertDeformType()) {
			case ARM_VDEF_BGE_CPU:
			{
				if (!shape_applied) {
					VerifyStorage();
				}
				DeformVerts();
				break;
			}
			case ARM_VDEF_BLENDER:
			{
				if (!shape_applied) {
					VerifyStorage();
				}
				BlenderDeformVerts();
				break;
			}
			case ARM_VDEF_BGE_GPU:
			{
				GPUDeformVerts();
				break;
			}
		}

		/* Update the current frame */
		m_lastArmaUpdate = m_armobj->GetLastFrame();

		/* dynamic vertex, cannot use display list */
		m_bDynamic = true;

		UpdateTransverts();

		/* indicate that the m_transverts and normals are up to date */
		return true;
	}

	return false;
}

bool BL_SkinDeformer::Update()
{
	return UpdateInternal(false);
}
