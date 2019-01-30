/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//r_alias.c -- alias model rendering

#include "quakedef.h"

extern cvar_t r_drawflat, gl_overbright_models, gl_fullbrights, r_lerpmodels, r_lerpmove; //johnfitz

//up to 16 color translated skins
gltexture_t *playertextures[MAX_SCOREBOARD]; //johnfitz -- changed to an array of pointers

#define NUMVERTEXNORMALS	162

float	r_avertexnormals[NUMVERTEXNORMALS][3] =
{
#include "anorms.h"
};

extern vec3_t	lightcolor; //johnfitz -- replaces "float shadelight" for lit support

// precalculated dot products for quantized angles
#define SHADEDOT_QUANT 16
float	r_avertexnormal_dots[SHADEDOT_QUANT][256] =
{
#include "anorm_dots.h"
};

extern	vec3_t			lightspot;

float	*shadedots = r_avertexnormal_dots[0];
vec3_t	shadevector;

float	entalpha; //johnfitz

qboolean	overbright; //johnfitz

qboolean shading = true; //johnfitz -- if false, disable vertex shading for various reasons (fullbright, r_lightmap, showtris, etc)

//johnfitz -- struct for passing lerp information to drawing functions
typedef struct {
	short pose1;
	short pose2;
	float blend;
	vec3_t origin;
	vec3_t angles;
} lerpdata_t;
//johnfitz

static GLuint r_alias_program;

// uniforms used in vert shader
static GLuint blendLoc;
static GLuint shadevectorLoc;
static GLuint lightColorLoc;

// uniforms used in frag shader
static GLuint texLoc;
static GLuint fullbrightTexLoc;
static GLuint useFullbrightTexLoc;
static GLuint useOverbrightLoc;
static GLuint useAlphaTestLoc;

#define pose1VertexAttrIndex 0
#define pose1NormalAttrIndex 1
#define pose2VertexAttrIndex 2
#define pose2NormalAttrIndex 3
#define texCoordsAttrIndex 4

/*
=============
GLARB_GetXYZOffset

Returns the offset of the first vertex's meshxyz_t.xyz in the vbo for the given
model and pose.
=============
*/
static void *GLARB_GetXYZOffset_MDL (aliashdr_t *hdr, int pose)
{
	const size_t xyzoffs = offsetof (meshxyz_mdl_t, xyz);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl_t)) + xyzoffs);
}
static void *GLARB_GetXYZOffset_MD3 (aliashdr_t *hdr, int pose)
{
	const size_t xyzoffs = offsetof (meshxyz_md3_t, xyz);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_md3_t)) + xyzoffs);
}

/*
=============
GLARB_GetNormalOffset

Returns the offset of the first vertex's meshxyz_t.normal in the vbo for the
given model and pose.
=============
*/
static void *GLARB_GetNormalOffset_MDL (aliashdr_t *hdr, int pose)
{
	const size_t normaloffs = offsetof (meshxyz_mdl_t, normal);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_mdl_t)) + normaloffs);
}
static void *GLARB_GetNormalOffset_MD3 (aliashdr_t *hdr, int pose)
{
	const size_t normaloffs = offsetof (meshxyz_md3_t, normal);
	return currententity->model->meshvboptr+(hdr->vbovertofs + (hdr->numverts_vbo * pose * sizeof (meshxyz_md3_t)) + normaloffs);
}

/*
=============
GLAlias_CreateShaders
=============
*/
void GLAlias_CreateShaders (void)
{
	const glsl_attrib_binding_t bindings[] = {
		{ "TexCoords", texCoordsAttrIndex },
		{ "Pose1Vert", pose1VertexAttrIndex },
		{ "Pose1Normal", pose1NormalAttrIndex },
		{ "Pose2Vert", pose2VertexAttrIndex },
		{ "Pose2Normal", pose2NormalAttrIndex }
	};

	const GLchar *vertSource = \
		"#version 110\n"
		"\n"
		"uniform float Blend;\n"
		"uniform vec3 ShadeVector;\n"
		"uniform vec4 LightColor;\n"
		"attribute vec4 TexCoords; // only xy are used \n"
		"attribute vec4 Pose1Vert;\n"
		"attribute vec3 Pose1Normal;\n"
		"attribute vec4 Pose2Vert;\n"
		"attribute vec3 Pose2Normal;\n"
		"\n"
		"varying float FogFragCoord;\n"
		"\n"
		"float r_avertexnormal_dot(vec3 vertexnormal) // from MH \n"
		"{\n"
		"        float dot = dot(vertexnormal, ShadeVector);\n"
		"        // wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case\n"
		"        if (dot < 0.0)\n"
		"            return 1.0 + dot * (13.0 / 44.0);\n"
		"        else\n"
		"            return 1.0 + dot;\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	gl_TexCoord[0] = TexCoords;\n"
		"	vec4 lerpedVert = mix(vec4(Pose1Vert.xyz, 1.0), vec4(Pose2Vert.xyz, 1.0), Blend);\n"
		"	gl_Position = gl_ModelViewProjectionMatrix * lerpedVert;\n"
		"	FogFragCoord = gl_Position.w;\n"
		"	float dot1 = r_avertexnormal_dot(Pose1Normal);\n"
		"	float dot2 = r_avertexnormal_dot(Pose2Normal);\n"
		"	gl_FrontColor = LightColor * vec4(vec3(mix(dot1, dot2, Blend)), 1.0);\n"
		"}\n";

	const GLchar *fragSource = \
		"#version 110\n"
		"\n"
		"uniform sampler2D Tex;\n"
		"uniform sampler2D FullbrightTex;\n"
		"uniform bool UseFullbrightTex;\n"
		"uniform bool UseOverbright;\n"
		"uniform bool UseAlphaTest;\n"
		"\n"
		"varying float FogFragCoord;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	vec4 result = texture2D(Tex, gl_TexCoord[0].xy);\n"
		"	if (UseAlphaTest && (result.a < 0.666))\n"
		"		discard;\n"
		"	result *= gl_Color;\n"
		"	if (UseOverbright)\n"
		"		result.rgb *= 2.0;\n"
		"	if (UseFullbrightTex)\n"
		"		result += texture2D(FullbrightTex, gl_TexCoord[0].xy);\n"
		"	result = clamp(result, 0.0, 1.0);\n"
		"	float fog = exp(-gl_Fog.density * gl_Fog.density * FogFragCoord * FogFragCoord);\n"
		"	fog = clamp(fog, 0.0, 1.0);\n"
		"	result = mix(gl_Fog.color, result, fog);\n"
		"	result.a = gl_Color.a;\n" // FIXME: This will make almost transparent things cut holes though heavy fog
		"	gl_FragColor = result;\n"
		"}\n";

	if (!gl_glsl_alias_able)
		return;

	r_alias_program = GL_CreateProgram (vertSource, fragSource, sizeof(bindings)/sizeof(bindings[0]), bindings);

	if (r_alias_program != 0)
	{
	// get uniform locations
		blendLoc = GL_GetUniformLocation (&r_alias_program, "Blend");
		shadevectorLoc = GL_GetUniformLocation (&r_alias_program, "ShadeVector");
		lightColorLoc = GL_GetUniformLocation (&r_alias_program, "LightColor");
		texLoc = GL_GetUniformLocation (&r_alias_program, "Tex");
		fullbrightTexLoc = GL_GetUniformLocation (&r_alias_program, "FullbrightTex");
		useFullbrightTexLoc = GL_GetUniformLocation (&r_alias_program, "UseFullbrightTex");
		useOverbrightLoc = GL_GetUniformLocation (&r_alias_program, "UseOverbright");
		useAlphaTestLoc = GL_GetUniformLocation (&r_alias_program, "UseAlphaTest");
	}
}

/*
=============
GL_DrawAliasFrame_GLSL -- ericw

Optimized alias model drawing codepath.
Compared to the original GL_DrawAliasFrame, this makes 1 draw call,
no vertex data is uploaded (it's already in the r_meshvbo and r_meshindexesvbo
static VBOs), and lerping and lighting is done in the vertex shader.

Supports optional overbright, optional fullbright pixels.

Based on code by MH from RMQEngine
=============
*/
void GL_DrawAliasFrame_GLSL (aliashdr_t *paliashdr, lerpdata_t lerpdata, gltexture_t *tx, gltexture_t *fb)
{
	float	blend;

	if (lerpdata.pose1 != lerpdata.pose2)
	{
		blend = lerpdata.blend;
	}
	else // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
	{
		blend = 0;
	}

	GL_UseProgramFunc (r_alias_program);

	GL_BindBuffer (GL_ARRAY_BUFFER, currententity->model->meshvbo);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, currententity->model->meshindexesvbo);

	GL_EnableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose1VertexAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose2VertexAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose1NormalAttrIndex);
	GL_EnableVertexAttribArrayFunc (pose2NormalAttrIndex);

	GL_VertexAttribPointerFunc (texCoordsAttrIndex, 2, GL_FLOAT, GL_FALSE, 0, currententity->model->meshvboptr+paliashdr->vbostofs);
	if (paliashdr->posevertssize == 1)
	{
		GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_mdl_t), GLARB_GetXYZOffset_MDL (paliashdr, lerpdata.pose1));
		GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (meshxyz_mdl_t), GLARB_GetXYZOffset_MDL (paliashdr, lerpdata.pose2));
		// GL_TRUE to normalize the signed bytes to [-1 .. 1]
		GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl_t), GLARB_GetNormalOffset_MDL (paliashdr, lerpdata.pose1));
		GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_mdl_t), GLARB_GetNormalOffset_MDL (paliashdr, lerpdata.pose2));
	}
	else if (paliashdr->posevertssize == 2)
	{
		GL_VertexAttribPointerFunc (pose1VertexAttrIndex, 4, GL_SHORT, GL_FALSE, sizeof (meshxyz_md3_t), GLARB_GetXYZOffset_MD3 (paliashdr, lerpdata.pose1));
		GL_VertexAttribPointerFunc (pose2VertexAttrIndex, 4, GL_SHORT, GL_FALSE, sizeof (meshxyz_md3_t), GLARB_GetXYZOffset_MD3 (paliashdr, lerpdata.pose2));
		// GL_TRUE to normalize the signed bytes to [-1 .. 1]
		GL_VertexAttribPointerFunc (pose1NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_md3_t), GLARB_GetNormalOffset_MD3 (paliashdr, lerpdata.pose1));
		GL_VertexAttribPointerFunc (pose2NormalAttrIndex, 4, GL_BYTE, GL_TRUE, sizeof (meshxyz_md3_t), GLARB_GetNormalOffset_MD3 (paliashdr, lerpdata.pose2));
	}

// set uniforms
	GL_Uniform1fFunc (blendLoc, blend);
	GL_Uniform3fFunc (shadevectorLoc, shadevector[0], shadevector[1], shadevector[2]);
	GL_Uniform4fFunc (lightColorLoc, lightcolor[0], lightcolor[1], lightcolor[2], entalpha);
	GL_Uniform1iFunc (texLoc, 0);
	GL_Uniform1iFunc (fullbrightTexLoc, 1);
	GL_Uniform1iFunc (useFullbrightTexLoc, (fb != NULL) ? 1 : 0);
	GL_Uniform1fFunc (useOverbrightLoc, overbright ? 1 : 0);
	GL_Uniform1iFunc (useAlphaTestLoc, (currententity->model->flags & MF_HOLEY) ? 1 : 0);

// set textures
	GL_SelectTexture (GL_TEXTURE0);
	GL_Bind (tx);

	if (fb)
	{
		GL_SelectTexture (GL_TEXTURE1);
		GL_Bind (fb);
	}

// draw
	glDrawElements (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, currententity->model->meshindexesvboptr+paliashdr->eboofs);

// clean up
	GL_DisableVertexAttribArrayFunc (texCoordsAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose1VertexAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose2VertexAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose1NormalAttrIndex);
	GL_DisableVertexAttribArrayFunc (pose2NormalAttrIndex);

	GL_UseProgramFunc (0);
	GL_SelectTexture (GL_TEXTURE0);

	rs_aliaspasses += paliashdr->numtris;
}

/*
=============
GL_DrawAliasFrame
-- johnfitz -- rewritten to support colored light, lerping, entalpha, multitexture, and r_drawflat
-- spike -- rewritten to use vertex arrays, which should be slightly faster thanks to less branches+gl calls (note that this requires gl1.1, which we depend on anyway for texture objects, and is pretty much universal.
=============
*/
void GL_DrawAliasFrame (aliashdr_t *paliashdr, lerpdata_t lerpdata)
{
	static	vec3_t vpos[65536];
	static	vec4_t vc[65536];
	int i;
	float	blend, iblend;

	if (lerpdata.pose1 != lerpdata.pose2)
	{
		blend = lerpdata.blend;
		iblend = 1.0-blend;
	}
	else // poses the same means either 1. the entity has paused its animation, or 2. r_lerpmodels is disabled
	{
		blend = 1;
		iblend = 0;
	}

	//pose1*iblend + pose2*blend

	if (shading && r_drawflat_cheatsafe)
	{
		shading = false;
		glColor4f (rand()%256/255.0, rand()%256/255.0, rand()%256/255.0, entalpha);
	}

	glEnableClientState(GL_VERTEX_ARRAY);
	if (paliashdr->posevertssize == 1)
	{
		trivertx_t *verts1 = (trivertx_t*)((byte *)paliashdr + paliashdr->vertexes) + lerpdata.pose1 * paliashdr->numverts_vbo;
		trivertx_t *verts2 = (trivertx_t*)((byte *)paliashdr + paliashdr->vertexes) + lerpdata.pose2 * paliashdr->numverts_vbo;

		if (iblend)
		{
			for (i = 0; i < paliashdr->numverts_vbo; i++)
			{
				vpos[i][0] = verts1[i].v[0] * iblend + blend * verts2[i].v[0];
				vpos[i][1] = verts1[i].v[1] * iblend + blend * verts2[i].v[1];
				vpos[i][2] = verts1[i].v[2] * iblend + blend * verts2[i].v[2];
			}
			GL_BindBuffer (GL_ARRAY_BUFFER, 0);
			glVertexPointer(3, GL_FLOAT, sizeof (vpos[0]), vpos);

			if (shading)
			{
				for (i = 0; i < paliashdr->numverts_vbo; i++)
				{
					vc[i][0] = (shadedots[verts1->lightnormalindex]*iblend + shadedots[verts2->lightnormalindex]*blend) * lightcolor[0];
					vc[i][1] = (shadedots[verts1->lightnormalindex]*iblend + shadedots[verts2->lightnormalindex]*blend) * lightcolor[1];
					vc[i][2] = (shadedots[verts1->lightnormalindex]*iblend + shadedots[verts2->lightnormalindex]*blend) * lightcolor[2];
					vc[i][3] = entalpha;
				}
				glEnableClientState(GL_COLOR_ARRAY);
				glColorPointer(4, GL_FLOAT, sizeof(*vc), vc);
			}
		}
		else
		{
			if (shading)
			{
				for (i = 0; i < paliashdr->numverts_vbo; i++)
				{
					vc[i][0] = shadedots[verts2->lightnormalindex] * lightcolor[0];
					vc[i][1] = shadedots[verts2->lightnormalindex] * lightcolor[1];
					vc[i][2] = shadedots[verts2->lightnormalindex] * lightcolor[2];
					vc[i][3] = entalpha;
				}
				glEnableClientState(GL_COLOR_ARRAY);
				GL_BindBuffer (GL_ARRAY_BUFFER, 0);
				glColorPointer(4, GL_FLOAT, 0, vc);
			}

			//glVertexPointer may not take GL_UNSIGNED_BYTE, which means we can't use our vbos. attribute 0 MAY be vertex coords, but I don't want to depend on that.
			for (i = 0; i < paliashdr->numverts_vbo; i++)
			{
				vpos[i][0] = verts2[i].v[0];
				vpos[i][1] = verts2[i].v[1];
				vpos[i][2] = verts2[i].v[2];
			}
			GL_BindBuffer (GL_ARRAY_BUFFER, 0);
			glVertexPointer(3, GL_FLOAT, sizeof (vpos[0]), vpos);
		}
	}
	else if (paliashdr->posevertssize == 2)
	{
		md3XyzNormal_t *verts1 = (md3XyzNormal_t*)((byte *)paliashdr + paliashdr->vertexes) + lerpdata.pose1 * paliashdr->numverts_vbo;
		md3XyzNormal_t *verts2 = (md3XyzNormal_t*)((byte *)paliashdr + paliashdr->vertexes) + lerpdata.pose2 * paliashdr->numverts_vbo;

		if (iblend)
		{
			for (i = 0; i < paliashdr->numverts_vbo; i++)
			{
				vpos[i][0] = verts1[i].xyz[0] * iblend + blend * verts2[i].xyz[0];
				vpos[i][1] = verts1[i].xyz[1] * iblend + blend * verts2[i].xyz[1];
				vpos[i][2] = verts1[i].xyz[2] * iblend + blend * verts2[i].xyz[2];
			}
			GL_BindBuffer (GL_ARRAY_BUFFER, 0);
			glVertexPointer(3, GL_FLOAT, sizeof (vpos[0]), vpos);

			if (shading)
			{
				for (i = 0; i < paliashdr->numverts_vbo; i++)
				{
					vec3_t n;
					float dot;
					// map the normal coordinates in [-1..1] to [-127..127] and store in an unsigned char.
					// this introduces some error (less than 0.004), but the normals were very coarse
					// to begin with
					//this should be a table.
					float lat = (float)verts2[i].latlong[0] * (2 * M_PI)*(1.0 / 255.0);
					float lng = (float)verts2[i].latlong[1] * (2 * M_PI)*(1.0 / 255.0);
					n[0] = blend * cos ( lng ) * sin ( lat );
					n[1] = blend * sin ( lng ) * sin ( lat );
					n[2] = blend * cos ( lat );
					lat = (float)verts1[i].latlong[0] * (2 * M_PI)*(1.0 / 255.0);
					lng = (float)verts1[i].latlong[1] * (2 * M_PI)*(1.0 / 255.0);
					n[0] += iblend * cos ( lng ) * sin ( lat );
					n[1] += iblend * sin ( lng ) * sin ( lat );
					n[2] += iblend * cos ( lat );
					dot = DotProduct(n, shadevector);
					if (dot < 0.0)	//bizzare maths guessed by mh
						dot = 1.0 + dot * (13.0 / 44.0);
					else
						dot = 1.0 + dot;
					vc[i][0] = dot * lightcolor[0];
					vc[i][1] = dot * lightcolor[1];
					vc[i][2] = dot * lightcolor[2];
					vc[i][3] = entalpha;
				}
				glEnableClientState(GL_COLOR_ARRAY);
				glColorPointer(4, GL_FLOAT, 0, vc);
			}
		}
		else
		{
			if (shading)
			{
				for (i = 0; i < paliashdr->numverts_vbo; i++)
				{
					vec3_t n;
					float dot;
					// map the normal coordinates in [-1..1] to [-127..127] and store in an unsigned char.
					// this introduces some error (less than 0.004), but the normals were very coarse
					// to begin with
					//this should be a table.
					float lat = (float)verts2[i].latlong[0] * (2 * M_PI)*(1.0 / 255.0);
					float lng = (float)verts2[i].latlong[1] * (2 * M_PI)*(1.0 / 255.0);
					n[0] = cos ( lng ) * sin ( lat );
					n[1] = sin ( lng ) * sin ( lat );
					n[2] = cos ( lat );
					dot = DotProduct(n, shadevector);
					if (dot < 0.0)	//bizzare maths guessed by mh
						dot = 1.0 + dot * (13.0 / 44.0);
					else
						dot = 1.0 + dot;
					vc[i][0] = dot * lightcolor[0];
					vc[i][1] = dot * lightcolor[1];
					vc[i][2] = dot * lightcolor[2];
					vc[i][3] = entalpha;
				}
				glEnableClientState(GL_COLOR_ARRAY);
				GL_BindBuffer (GL_ARRAY_BUFFER, 0);
				glColorPointer(4, GL_FLOAT, 0, vc);
			}
			GL_BindBuffer (GL_ARRAY_BUFFER, currententity->model->meshvbo);
			glVertexPointer(3, GL_SHORT, sizeof (meshxyz_md3_t), GLARB_GetXYZOffset_MD3 (paliashdr, lerpdata.pose2));
		}
	}

// set textures
	GL_BindBuffer (GL_ARRAY_BUFFER, currententity->model->meshvbo);
	if (mtexenabled)
	{
		GL_ClientActiveTextureFunc (GL_TEXTURE0);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 0, currententity->model->meshvboptr+paliashdr->vbostofs);

		GL_ClientActiveTextureFunc (GL_TEXTURE1);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 0, currententity->model->meshvboptr+paliashdr->vbostofs);
	}
	else
	{
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, 0, currententity->model->meshvboptr+paliashdr->vbostofs);
	}

// draw
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, currententity->model->meshindexesvbo);
	glDrawElements (GL_TRIANGLES, paliashdr->numindexes, GL_UNSIGNED_SHORT, currententity->model->meshindexesvboptr + paliashdr->eboofs);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);

	GL_BindBuffer (GL_ARRAY_BUFFER, 0);

// clean up
	if (mtexenabled)
	{
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		GL_ClientActiveTextureFunc (GL_TEXTURE0);
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);


	rs_aliaspasses += paliashdr->numtris;
}

/*
=================
R_SetupAliasFrame -- johnfitz -- rewritten to support lerping
=================
*/
void R_SetupAliasFrame (aliashdr_t *paliashdr, int frame, lerpdata_t *lerpdata)
{
	entity_t		*e = currententity;
	int				posenum, numposes;

	if ((frame >= paliashdr->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d for '%s'\n", frame, e->model->name);
		frame = 0;
	}

	posenum = paliashdr->frames[frame].firstpose;
	numposes = paliashdr->frames[frame].numposes;

	if (numposes > 1)
	{
		e->lerptime = paliashdr->frames[frame].interval;
		posenum += (int)(cl.time / e->lerptime) % numposes;
	}
	else
		e->lerptime = 0.1;

	if (e->lerpflags & LERP_RESETANIM) //kill any lerp in progress
	{
		e->lerpstart = 0;
		e->previouspose = posenum;
		e->currentpose = posenum;
		e->lerpflags -= LERP_RESETANIM;
	}
	else if (e->currentpose != posenum) // pose changed, start new lerp
	{
		if (e->lerpflags & LERP_RESETANIM2) //defer lerping one more time
		{
			e->lerpstart = 0;
			e->previouspose = posenum;
			e->currentpose = posenum;
			e->lerpflags -= LERP_RESETANIM2;
		}
		else
		{
			e->lerpstart = cl.time;
			e->previouspose = e->currentpose;
			e->currentpose = posenum;
		}
	}

	//set up values
	if (r_lerpmodels.value && !(e->model->flags & MOD_NOLERP && r_lerpmodels.value != 2))
	{
		if (e->lerpflags & LERP_FINISH && numposes == 1)
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / (e->lerpfinish - e->lerpstart), 1);
		else
			lerpdata->blend = CLAMP (0, (cl.time - e->lerpstart) / e->lerptime, 1);
		lerpdata->pose1 = e->previouspose;
		lerpdata->pose2 = e->currentpose;
	}
	else //don't lerp
	{
		lerpdata->blend = 1;
		lerpdata->pose1 = posenum;
		lerpdata->pose2 = posenum;
	}
}

/*
=================
R_SetupEntityTransform -- johnfitz -- set up transform part of lerpdata
=================
*/
void R_SetupEntityTransform (entity_t *e, lerpdata_t *lerpdata)
{
	float blend;
	vec3_t d;
	int i;

	// if LERP_RESETMOVE, kill any lerps in progress
	if (e->lerpflags & LERP_RESETMOVE)
	{
		e->movelerpstart = 0;
		VectorCopy (e->origin, e->previousorigin);
		VectorCopy (e->origin, e->currentorigin);
		VectorCopy (e->angles, e->previousangles);
		VectorCopy (e->angles, e->currentangles);
		e->lerpflags -= LERP_RESETMOVE;
	}
	else if (!VectorCompare (e->origin, e->currentorigin) || !VectorCompare (e->angles, e->currentangles)) // origin/angles changed, start new lerp
	{
		e->movelerpstart = cl.time;
		VectorCopy (e->currentorigin, e->previousorigin);
		VectorCopy (e->origin,  e->currentorigin);
		VectorCopy (e->currentangles, e->previousangles);
		VectorCopy (e->angles,  e->currentangles);
	}

	//set up values
	if (r_lerpmove.value && e != &cl.viewent && e->lerpflags & LERP_MOVESTEP)
	{
		if (e->lerpflags & LERP_FINISH)
			blend = CLAMP (0, (cl.time - e->movelerpstart) / (e->lerpfinish - e->movelerpstart), 1);
		else
			blend = CLAMP (0, (cl.time - e->movelerpstart) / 0.1, 1);

		//translation
		VectorSubtract (e->currentorigin, e->previousorigin, d);
		lerpdata->origin[0] = e->previousorigin[0] + d[0] * blend;
		lerpdata->origin[1] = e->previousorigin[1] + d[1] * blend;
		lerpdata->origin[2] = e->previousorigin[2] + d[2] * blend;

		//rotation
		VectorSubtract (e->currentangles, e->previousangles, d);
		for (i = 0; i < 3; i++)
		{
			if (d[i] > 180)  d[i] -= 360;
			if (d[i] < -180) d[i] += 360;
		}
		lerpdata->angles[0] = e->previousangles[0] + d[0] * blend;
		lerpdata->angles[1] = e->previousangles[1] + d[1] * blend;
		lerpdata->angles[2] = e->previousangles[2] + d[2] * blend;
	}
	else //don't lerp
	{
		VectorCopy (e->origin, lerpdata->origin);
		VectorCopy (e->angles, lerpdata->angles);
	}
}

/*
=================
R_SetupAliasLighting -- johnfitz -- broken out from R_DrawAliasModel and rewritten
=================
*/
void R_SetupAliasLighting (entity_t	*e)
{
	vec3_t		dist;
	float		add;
	int			i;
	int		quantizedangle;
	float		radiansangle;
	float		*origin = e->origin;

	if (e->eflags & EFLAGS_VIEWMODEL)
		origin = r_refdef.vieworg;
	R_LightPoint (origin);

	//add dlights
	for (i=0 ; i<MAX_DLIGHTS ; i++)
	{
		if (cl_dlights[i].die >= cl.time)
		{
			VectorSubtract (origin, cl_dlights[i].origin, dist);
			add = cl_dlights[i].radius - VectorLength(dist);
			if (add > 0)
				VectorMA (lightcolor, add, cl_dlights[i].color, lightcolor);
		}
	}

	// minimum light value on gun (24)
	if (e == &cl.viewent)
	{
		add = 72.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			lightcolor[0] += add / 3.0f;
			lightcolor[1] += add / 3.0f;
			lightcolor[2] += add / 3.0f;
		}
	}

	// minimum light value on players (8)
	if (e > cl.entities && e <= cl.entities + cl.maxclients)
	{
		add = 24.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			lightcolor[0] += add / 3.0f;
			lightcolor[1] += add / 3.0f;
			lightcolor[2] += add / 3.0f;
		}
	}

	// clamp lighting so it doesn't overbright as much (96)
	if (overbright)
	{
		add = 288.0f / (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add < 1.0f)
			VectorScale(lightcolor, add, lightcolor);
	}

	//hack up the brightness when fullbrights but no overbrights (256)
	if (gl_fullbrights.value && !gl_overbright_models.value)
		if (e->model->flags & MOD_FBRIGHTHACK)
		{
			lightcolor[0] = 256.0f;
			lightcolor[1] = 256.0f;
			lightcolor[2] = 256.0f;
		}

	quantizedangle = ((int)(e->angles[1] * (SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);

//ericw -- shadevector is passed to the shader to compute shadedots inside the
//shader, see GLAlias_CreateShaders()
	radiansangle = (quantizedangle / 16.0) * 2.0 * 3.14159;
	shadevector[0] = cos(-radiansangle);
	shadevector[1] = sin(-radiansangle);
	shadevector[2] = 1;
	VectorNormalize(shadevector);
//ericw --

	shadedots = r_avertexnormal_dots[quantizedangle];
	VectorScale (lightcolor, 1.0f / 200.0f, lightcolor);

	lightcolor[0] *= e->netstate.colormod[0] / 32.0;
	lightcolor[1] *= e->netstate.colormod[1] / 32.0;
	lightcolor[2] *= e->netstate.colormod[2] / 32.0;
}

/*
=================
R_DrawAliasModel -- johnfitz -- almost completely rewritten
=================
*/
void R_DrawAliasModel (entity_t *e)
{
	aliashdr_t	*paliashdr;
	int			i, anim;
	gltexture_t	*tx, *fb;
	lerpdata_t	lerpdata;
	qboolean	alphatest = !!(e->model->flags & MF_HOLEY);
	int surf;

	//
	// setup pose/lerp data -- do it first so we don't miss updates due to culling
	//
	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	if (e->eflags & EFLAGS_VIEWMODEL)
	{
		//transform it relative to the view, by rebuilding the modelview matrix without the view position.
		glPushMatrix ();
		glLoadIdentity();
		glRotatef (-90,  1, 0, 0);	    // put Z going up
		glRotatef (90,  0, 0, 1);	    // put Z going up

		glDepthRange (0, 0.3);
	}
	else
	{
		//
		// cull it
		//
		if (R_CullModelForEntity(e))
			return;

		//
		// transform it
		//
		glPushMatrix ();
	}
	R_RotateForEntity (lerpdata.origin, lerpdata.angles, e->netstate.scale);
	glTranslatef (paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
	glScalef (paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);

	//
	// random stuff
	//
	if (gl_smoothmodels.value && !r_drawflat_cheatsafe)
		glShadeModel (GL_SMOOTH);
	if (gl_affinemodels.value)
		glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
	overbright = gl_overbright_models.value;
	shading = true;

	//
	// set up for alpha blending
	//
	if (r_drawflat_cheatsafe || r_lightmap_cheatsafe) //no alpha in drawflat or lightmap mode
		entalpha = 1;
	else
		entalpha = ENTALPHA_DECODE(e->alpha);
	if (entalpha == 0)
		goto cleanup;
	if (entalpha < 1)
	{
		if (!gl_texture_env_combine) overbright = false; //overbright can't be done in a single pass without combiners
		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
	}
	else if (alphatest)
		glEnable (GL_ALPHA_TEST);

	//
	// set up lighting
	//
	R_SetupAliasLighting (e);

	for(surf=0;;surf++)
	{
		rs_aliaspolys += paliashdr->numtris;

		//
		// set up textures
		//
		GL_DisableMultitexture();
		anim = (int)(cl.time*10) & 3;
		if ((e->skinnum >= paliashdr->numskins) || (e->skinnum < 0))
		{
			Con_DPrintf ("R_DrawAliasModel: no such skin # %d for '%s'\n", e->skinnum, e->model->name);
			tx = NULL; // NULL will give the checkerboard texture
			fb = NULL;
		}
		else
		{
			tx = paliashdr->gltextures[e->skinnum][anim];
			fb = paliashdr->fbtextures[e->skinnum][anim];
		} 
		if (e->colormap != vid.colormap && !gl_nocolors.value)
		{
			i = e - cl.entities;
			if (i >= 1 && i<=cl.maxclients /* && !strcmp (currententity->model->name, "progs/player.mdl") */)
				tx = playertextures[i - 1];
		}
		if (!gl_fullbrights.value)
			fb = NULL;

		//
		// draw it
		//
		if (r_drawflat_cheatsafe)
		{
			glDisable (GL_TEXTURE_2D);
			GL_DrawAliasFrame (paliashdr, lerpdata);
			glEnable (GL_TEXTURE_2D);
			srand((int) (cl.time * 1000)); //restore randomness
		}
		else if (r_fullbright_cheatsafe)
		{
			GL_Bind (tx);
			shading = false;
			glColor4f(1,1,1,entalpha);
			GL_DrawAliasFrame (paliashdr, lerpdata);
			if (fb)
			{
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				GL_Bind(fb);
				glEnable(GL_BLEND);
				glBlendFunc (GL_ONE, GL_ONE);
				glDepthMask(GL_FALSE);
				glColor3f(entalpha,entalpha,entalpha);
				Fog_StartAdditive ();
				GL_DrawAliasFrame (paliashdr, lerpdata);
				Fog_StopAdditive ();
				glDepthMask(GL_TRUE);
				glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDisable(GL_BLEND);
			}
		}
		else if (r_lightmap_cheatsafe)
		{
			glDisable (GL_TEXTURE_2D);
			shading = false;
			glColor3f(1,1,1);
			GL_DrawAliasFrame (paliashdr, lerpdata);
			glEnable (GL_TEXTURE_2D);
		}
	// call fast path if possible. if the shader compliation failed for some reason,
	// r_alias_program will be 0.
		else if (r_alias_program != 0)
		{
			GL_DrawAliasFrame_GLSL (paliashdr, lerpdata, tx, fb);
		}
		else if (overbright)
		{
			if  (gl_texture_env_combine && gl_mtexable && gl_texture_env_add && fb) //case 1: everything in one pass
			{
				GL_Bind (tx);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 2.0f);
				GL_EnableMultitexture(); // selects TEXTURE1
				GL_Bind (fb);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
//				glEnable(GL_BLEND);
				GL_DrawAliasFrame (paliashdr, lerpdata);
//				glDisable(GL_BLEND);
				GL_DisableMultitexture();
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			}
			else if (gl_texture_env_combine) //case 2: overbright in one pass, then fullbright pass
			{
			// first pass
				GL_Bind(tx);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
				glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
				glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 2.0f);
				GL_DrawAliasFrame (paliashdr, lerpdata);
				glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			// second pass
				if (fb)
				{
					glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					GL_Bind(fb);
					glEnable(GL_BLEND);
					glBlendFunc (GL_ONE, GL_ONE);
					glDepthMask(GL_FALSE);
					shading = false;
					glColor3f(entalpha,entalpha,entalpha);
					Fog_StartAdditive ();
					GL_DrawAliasFrame (paliashdr, lerpdata);
					Fog_StopAdditive ();
					glDepthMask(GL_TRUE);
					glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					glDisable(GL_BLEND);
					glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
				}
			}
			else //case 3: overbright in two passes, then fullbright pass
			{
			// first pass
				GL_Bind(tx);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				GL_DrawAliasFrame (paliashdr, lerpdata);
			// second pass -- additive with black fog, to double the object colors but not the fog color
				glEnable(GL_BLEND);
				glBlendFunc (GL_ONE, GL_ONE);
				glDepthMask(GL_FALSE);
				Fog_StartAdditive ();
				GL_DrawAliasFrame (paliashdr, lerpdata);
				Fog_StopAdditive ();
				glDepthMask(GL_TRUE);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDisable(GL_BLEND);
			// third pass
				if (fb)
				{
					glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					GL_Bind(fb);
					glEnable(GL_BLEND);
					glBlendFunc (GL_ONE, GL_ONE);
					glDepthMask(GL_FALSE);
					shading = false;
					glColor3f(entalpha,entalpha,entalpha);
					Fog_StartAdditive ();
					GL_DrawAliasFrame (paliashdr, lerpdata);
					Fog_StopAdditive ();
					glDepthMask(GL_TRUE);
					glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					glDisable(GL_BLEND);
					glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
				}
			}
		}
		else
		{
			if (gl_mtexable && gl_texture_env_add && fb) //case 4: fullbright mask using multitexture
			{
				GL_DisableMultitexture(); // selects TEXTURE0
				GL_Bind (tx);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				GL_EnableMultitexture(); // selects TEXTURE1
				GL_Bind (fb);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
				glEnable(GL_BLEND);
				GL_DrawAliasFrame (paliashdr, lerpdata);
				glDisable(GL_BLEND);
				GL_DisableMultitexture();
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
			}
			else //case 5: fullbright mask without multitexture
			{
			// first pass
				GL_Bind(tx);
				glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				GL_DrawAliasFrame (paliashdr, lerpdata);
			// second pass
				if (fb)
				{
					GL_Bind(fb);
					glEnable(GL_BLEND);
					glBlendFunc (GL_ONE, GL_ONE);
					glDepthMask(GL_FALSE);
					shading = false;
					glColor3f(entalpha,entalpha,entalpha);
					Fog_StartAdditive ();
					GL_DrawAliasFrame (paliashdr, lerpdata);
					Fog_StopAdditive ();
					glDepthMask(GL_TRUE);
					glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					glDisable(GL_BLEND);
				}
			}
		}
		if (!paliashdr->nextsurface)
			break;
		paliashdr = (aliashdr_t*)((byte*)paliashdr + paliashdr->nextsurface);
	}

cleanup:
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glHint (GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glShadeModel (GL_FLAT);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	if (alphatest)
		glDisable (GL_ALPHA_TEST);
	glColor3f(1,1,1);
	if (e->eflags & EFLAGS_VIEWMODEL)
		glDepthRange (0, 1);
	glPopMatrix ();
}

//johnfitz -- values for shadow matrix
#define SHADOW_SKEW_X -0.7 //skew along x axis. -0.7 to mimic glquake shadows
#define SHADOW_SKEW_Y 0 //skew along y axis. 0 to mimic glquake shadows
#define SHADOW_VSCALE 0 //0=completely flat
#define SHADOW_HEIGHT 0.1 //how far above the floor to render the shadow
//johnfitz

/*
=============
GL_DrawAliasShadow -- johnfitz -- rewritten

TODO: orient shadow onto "lightplane" (a global mplane_t*)
=============
*/
void GL_DrawAliasShadow (entity_t *e)
{
	float	shadowmatrix[16] = {1,				0,				0,				0,
								0,				1,				0,				0,
								SHADOW_SKEW_X,	SHADOW_SKEW_Y,	SHADOW_VSCALE,	0,
								0,				0,				SHADOW_HEIGHT,	1};
	float		lheight;
	aliashdr_t	*paliashdr;
	lerpdata_t	lerpdata;

	if (R_CullModelForEntity(e))
		return;

	if (e == &cl.viewent || e->effects & EF_NOSHADOW || e->model->flags & MOD_NOSHADOW)
		return;

	entalpha = ENTALPHA_DECODE(e->alpha);
	if (entalpha == 0) return;

	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);
	R_LightPoint (e->origin);
	lheight = currententity->origin[2] - lightspot[2];

// set up matrix
	glPushMatrix ();
	glTranslatef (lerpdata.origin[0],  lerpdata.origin[1],  lerpdata.origin[2]);
	glTranslatef (0,0,-lheight);
	glMultMatrixf (shadowmatrix);
	glTranslatef (0,0,lheight);
	glRotatef (lerpdata.angles[1],  0, 0, 1);
	glRotatef (-lerpdata.angles[0],  0, 1, 0);
	glRotatef (lerpdata.angles[2],  1, 0, 0);
	glTranslatef (paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
	glScalef (paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);

// draw it
	glDepthMask(GL_FALSE);
	glEnable (GL_BLEND);
	GL_DisableMultitexture ();
	glDisable (GL_TEXTURE_2D);
	shading = false;
	glColor4f(0,0,0,entalpha * 0.5);
	GL_DrawAliasFrame (paliashdr, lerpdata);
	glEnable (GL_TEXTURE_2D);
	glDisable (GL_BLEND);
	glDepthMask(GL_TRUE);

//clean up
	glPopMatrix ();
}

/*
=================
R_DrawAliasModel_ShowTris -- johnfitz
=================
*/
void R_DrawAliasModel_ShowTris (entity_t *e)
{
	aliashdr_t	*paliashdr;
	lerpdata_t	lerpdata;

	if (R_CullModelForEntity(e))
		return;

	paliashdr = (aliashdr_t *)Mod_Extradata (e->model);
	R_SetupAliasFrame (paliashdr, e->frame, &lerpdata);
	R_SetupEntityTransform (e, &lerpdata);

	glPushMatrix ();
	R_RotateForEntity (lerpdata.origin,lerpdata.angles, e->netstate.scale);
	glTranslatef (paliashdr->scale_origin[0], paliashdr->scale_origin[1], paliashdr->scale_origin[2]);
	glScalef (paliashdr->scale[0], paliashdr->scale[1], paliashdr->scale[2]);

	shading = false;
	glColor3f(1,1,1);
	GL_DrawAliasFrame (paliashdr, lerpdata);

	glPopMatrix ();
}

