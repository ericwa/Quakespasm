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
// gl_mesh.c: triangle model functions

#include "quakedef.h"


/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

#define countof(x) (sizeof(x)/sizeof((x)[0]))

/*
================
GL_MakeAliasModelDisplayLists

Saves data needed to build the VBO for this model on the hunk. Afterwards this
is copied to Mod_Extradata.

Original code by MH from RMQEngine
================
*/
void GL_MakeAliasModelDisplayLists (qmodel_t *m, aliashdr_t *paliashdr)
{
	int i, j;
	int maxverts_vbo;
	unsigned short *indexes;
	trivertx_t *verts;
	aliasmesh_t *desc;

	// there can never be more than this number of verts and we just put them all on the hunk
	//	front/back logic says we can never have more than numverts*2
	maxverts_vbo = paliashdr->numverts * 2;
	desc = (aliasmesh_t *) Hunk_Alloc (sizeof (aliasmesh_t) * maxverts_vbo);

	// there will always be this number of indexes
	indexes = (unsigned short *) Hunk_Alloc (sizeof (unsigned short) * paliashdr->numtris * 3);

	paliashdr->indexes = (intptr_t) indexes - (intptr_t) paliashdr;
	paliashdr->meshdesc = (intptr_t) desc - (intptr_t) paliashdr;
	paliashdr->numindexes = 0;
	paliashdr->numverts_vbo = 0;

	for (i = 0; i < paliashdr->numtris; i++)
	{
		for (j = 0; j < 3; j++)
		{
			int v;

			// index into hdr->vertexes
			unsigned short vertindex = triangles[i].vertindex[j];

			// basic s/t coords
			int s = stverts[vertindex].s;
			int t = stverts[vertindex].t;

			// check for back side and adjust texcoord s
			if (!triangles[i].facesfront && stverts[vertindex].onseam) s += paliashdr->skinwidth / 2;

			// see does this vert already exist
			for (v = 0; v < paliashdr->numverts_vbo; v++)
			{
				// it could use the same xyz but have different s and t
				if (desc[v].vertindex == vertindex && (int) desc[v].st[0] == s && (int) desc[v].st[1] == t)
				{
					// exists; emit an index for it
					indexes[paliashdr->numindexes++] = v;

					// no need to check any more
					break;
				}
			}

			if (v == paliashdr->numverts_vbo)
			{
				// doesn't exist; emit a new vert and index
				indexes[paliashdr->numindexes++] = paliashdr->numverts_vbo;

				desc[paliashdr->numverts_vbo].vertindex = vertindex;
				desc[paliashdr->numverts_vbo].st[0] = s;
				desc[paliashdr->numverts_vbo++].st[1] = t;
			}
		}
	}

	switch(paliashdr->poseverttype)
	{
	case PV_QUAKEFORGE:
		verts = (trivertx_t *) Hunk_Alloc (paliashdr->numposes * paliashdr->numverts_vbo*2 * sizeof(*verts));
		paliashdr->vertexes = (byte *)verts - (byte *)paliashdr;
		for (i=0 ; i<paliashdr->numposes ; i++)
			for (j=0 ; j<paliashdr->numverts_vbo ; j++)
			{
				verts[i*paliashdr->numverts_vbo*2 + j] = poseverts_mdl[i][desc[j].vertindex];
				verts[i*paliashdr->numverts_vbo*2 + j + paliashdr->numverts_vbo] = poseverts_mdl[i][desc[j].vertindex + paliashdr->numverts_vbo];
			}
		break;
	case PV_QUAKE1:
		verts = (trivertx_t *) Hunk_Alloc (paliashdr->numposes * paliashdr->numverts_vbo * sizeof(*verts));
		paliashdr->vertexes = (byte *)verts - (byte *)paliashdr;
		for (i=0 ; i<paliashdr->numposes ; i++)
			for (j=0 ; j<paliashdr->numverts_vbo ; j++)
				verts[i*paliashdr->numverts_vbo + j] = poseverts_mdl[i][desc[j].vertindex];
		break;
	case PV_QUAKE3:
		break;	//invalid here.
	}
}

#define NUMVERTEXNORMALS	 162
extern	float	r_avertexnormals[NUMVERTEXNORMALS][3];

/*
================
GLMesh_LoadVertexBuffer

Upload the given alias model's mesh to a VBO

Original code by MH from RMQEngine

may update the mesh vbo/ebo offsets.
================
*/
void GLMesh_LoadVertexBuffer (qmodel_t *m, aliashdr_t *mainhdr)
{
	//we always need vertex array data.
	//if we don't support vbos(gles?) then we just use system memory.
	//if we're not using glsl(gles1?), then we don't actually need all the data, but we do still need some so its easier to just alloc the lot.
	int totalvbosize = 0;
	const aliasmesh_t *desc;
	const void *trivertexes;
	byte *ebodata;
	byte *vbodata;
	int f;
	aliashdr_t *hdr;
	unsigned int numindexes, numverts;
	intptr_t stofs;
	intptr_t vertofs;

	//count how much space we're going to need.
	for(hdr = mainhdr, numverts = 0, numindexes = 0; ; )
	{
		switch(hdr->poseverttype)
		{
		case PV_QUAKE1:
			totalvbosize += (hdr->numposes * hdr->numverts_vbo * sizeof (meshxyz_mdl_t)); // ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm
			break;
		case PV_QUAKEFORGE:
			totalvbosize += (hdr->numposes * hdr->numverts_vbo * sizeof (meshxyz_mdl16_t));
			break;
		case PV_QUAKE3:
			totalvbosize += (hdr->numposes * hdr->numverts_vbo * sizeof (meshxyz_md3_t));
			break;
		}

		numverts += hdr->numverts_vbo;
		numindexes += hdr->numindexes;

		if (hdr->nextsurface)
			hdr = (aliashdr_t*)((byte*)hdr + hdr->nextsurface);
		else
			break;
	}
	hdr = NULL;

	vertofs = 0;
	totalvbosize = (totalvbosize+7)&~7;	//align it.
	stofs = totalvbosize;
	totalvbosize += (numverts * sizeof (meshst_t));

	if (!totalvbosize) return;
	if (!numindexes) return;

	//create an elements buffer
	ebodata = (byte *) malloc(numindexes * sizeof(unsigned short));
	if (!ebodata)
		return;	//fatal

	// create the vertex buffer (empty)
	vbodata = (byte *) malloc(totalvbosize);
	if (!vbodata)
	{	//fatal
		free(ebodata);
		return;
	}
	memset(vbodata, 0, totalvbosize);

	numindexes = 0;

	for(hdr = mainhdr, numverts = 0, numindexes = 0; ; )
	{
		// grab the pointers to data in the extradata
		desc = (aliasmesh_t *) ((byte *) hdr + hdr->meshdesc);
		trivertexes = (void *) ((byte *)hdr + hdr->vertexes);

		//submit the index data.
		hdr->eboofs = numindexes * sizeof (unsigned short);
		numindexes += hdr->numindexes;
		memcpy(ebodata + hdr->eboofs, (short *) ((byte *) hdr + hdr->indexes), hdr->numindexes * sizeof (unsigned short));

		hdr->vbovertofs = vertofs;

	// fill in the vertices at the start of the buffer
		switch(hdr->poseverttype)
		{
		case PV_QUAKE1:
			for (f = 0; f < hdr->numposes; f++) // ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm
			{
				int v;
				meshxyz_mdl_t *xyz = (meshxyz_mdl_t *) (vbodata + vertofs);
				const trivertx_t *tv = (trivertx_t*)trivertexes + (hdr->numverts_vbo * f);
				vertofs += hdr->numverts_vbo * sizeof (*xyz);

				for (v = 0; v < hdr->numverts_vbo; v++, tv++)
				{
					xyz[v].xyz[0] = tv->v[0];
					xyz[v].xyz[1] = tv->v[1];
					xyz[v].xyz[2] = tv->v[2];
					xyz[v].xyz[3] = 1;	// need w 1 for 4 byte vertex compression

					// map the normal coordinates in [-1..1] to [-127..127] and store in an unsigned char.
					// this introduces some error (less than 0.004), but the normals were very coarse
					// to begin with
					xyz[v].normal[0] = 127 * r_avertexnormals[tv->lightnormalindex][0];
					xyz[v].normal[1] = 127 * r_avertexnormals[tv->lightnormalindex][1];
					xyz[v].normal[2] = 127 * r_avertexnormals[tv->lightnormalindex][2];
					xyz[v].normal[3] = 0;	// unused; for 4-byte alignment
				}
			}
			break;
		case PV_QUAKEFORGE:
			for (f = 0; f < hdr->numposes; f++) // ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm
			{
				int v;
				meshxyz_mdl16_t *xyz = (meshxyz_mdl16_t *) (vbodata + vertofs);
				const trivertx_t *tv = (trivertx_t*)trivertexes + (hdr->numverts_vbo*2 * f);
				vertofs += hdr->numverts_vbo * sizeof (*xyz);

				for (v = 0; v < hdr->numverts_vbo; v++, tv++)
				{
					xyz[v].xyz[0] = (tv->v[0]<<8) | tv[hdr->numverts_vbo].v[0];
					xyz[v].xyz[1] = (tv->v[1]<<8) | tv[hdr->numverts_vbo].v[0];
					xyz[v].xyz[2] = (tv->v[2]<<8) | tv[hdr->numverts_vbo].v[0];
					xyz[v].xyz[3] = 1;	// need w 1 for 4 byte vertex compression

					// map the normal coordinates in [-1..1] to [-127..127] and store in an unsigned char.
					// this introduces some error (less than 0.004), but the normals were very coarse
					// to begin with
					xyz[v].normal[0] = 127 * r_avertexnormals[tv->lightnormalindex][0];
					xyz[v].normal[1] = 127 * r_avertexnormals[tv->lightnormalindex][1];
					xyz[v].normal[2] = 127 * r_avertexnormals[tv->lightnormalindex][2];
					xyz[v].normal[3] = 0;	// unused; for 4-byte alignment
				}
			}
			break;
		case PV_QUAKE3:
			for (f = 0; f < hdr->numposes; f++) // ericw -- what RMQEngine called nummeshframes is called numposes in QuakeSpasm
			{
				int v;
				meshxyz_md3_t *xyz = (meshxyz_md3_t *) (vbodata + vertofs);
				const md3XyzNormal_t *tv = (md3XyzNormal_t*)trivertexes + (hdr->numverts_vbo * f);
				float lat,lng;
				vertofs += hdr->numverts_vbo * sizeof (*xyz);

				for (v = 0; v < hdr->numverts_vbo; v++, tv++)
				{
					xyz[v].xyz[0] = tv->xyz[0];
					xyz[v].xyz[1] = tv->xyz[1];
					xyz[v].xyz[2] = tv->xyz[2];
					xyz[v].xyz[3] = 1;	// need w 1 for 4 byte vertex compression

					// map the normal coordinates in [-1..1] to [-127..127] and store in an unsigned char.
					// this introduces some error (less than 0.004), but the normals were very coarse
					// to begin with
					lat = (float)tv->latlong[0] * (2 * M_PI)*(1.0 / 255.0);
					lng = (float)tv->latlong[1] * (2 * M_PI)*(1.0 / 255.0);
					xyz[v].normal[0] = 127 * cos ( lng ) * sin ( lat );
					xyz[v].normal[1] = 127 * sin ( lng ) * sin ( lat );
					xyz[v].normal[2] = 127 * cos ( lat );
					xyz[v].normal[3] = 0;	// unused; for 4-byte alignment
				}
			}
			break;
		}

		// fill in the ST coords at the end of the buffer
		{
			meshst_t *st;
			float hscale, vscale;

			//johnfitz -- padded skins
			hscale = (float)hdr->skinwidth/(float)TexMgr_PadConditional(hdr->skinwidth);
			vscale = (float)hdr->skinheight/(float)TexMgr_PadConditional(hdr->skinheight);
			//johnfitz

			hdr->vbostofs = stofs; 
			st = (meshst_t *) (vbodata + stofs);
			stofs += hdr->numverts_vbo*sizeof(*st);
			switch(hdr->poseverttype)
			{
			case PV_QUAKE3:
				for (f = 0; f < hdr->numverts_vbo; f++)
				{	//md3 has floating-point skin coords. use the values directly.
					st[f].st[0] = hscale * desc[f].st[0];
					st[f].st[1] = vscale * desc[f].st[1];
				}
				break;
			case PV_QUAKEFORGE:
			case PV_QUAKE1:
				for (f = 0; f < hdr->numverts_vbo; f++)
				{
					st[f].st[0] = hscale * ((float) desc[f].st[0] + 0.5f) / (float) hdr->skinwidth;
					st[f].st[1] = vscale * ((float) desc[f].st[1] + 0.5f) / (float) hdr->skinheight;
				}
				break;
			}
		}

		if (hdr->nextsurface)
			hdr = (aliashdr_t*)((byte*)hdr + hdr->nextsurface);
		else
			break;
	}
	hdr = NULL;

	if (gl_vbo_able)
	{
		// upload indexes buffer
		GL_DeleteBuffersFunc (1, &m->meshindexesvbo);
		GL_GenBuffersFunc (1, &m->meshindexesvbo);
		GL_BindBufferFunc (GL_ELEMENT_ARRAY_BUFFER, m->meshindexesvbo);
		GL_BufferDataFunc (GL_ELEMENT_ARRAY_BUFFER, numindexes * sizeof (unsigned short), ebodata, GL_STATIC_DRAW);

		// upload vertexes buffer
		GL_DeleteBuffersFunc (1, &m->meshvbo);
		GL_GenBuffersFunc (1, &m->meshvbo);
		GL_BindBufferFunc (GL_ARRAY_BUFFER, m->meshvbo);
		GL_BufferDataFunc (GL_ARRAY_BUFFER, totalvbosize, vbodata, GL_STATIC_DRAW);

		free (vbodata);
		free (ebodata);

		m->meshvboptr = NULL;
		m->meshindexesvboptr = NULL;
	}
	else
	{
		m->meshvboptr = vbodata;
		m->meshindexesvboptr = ebodata;
	}

// invalidate the cached bindings
	GL_ClearBufferBindings ();
}

/*
================
GLMesh_LoadVertexBuffers

Loop over all precached alias models, and upload each one to a VBO.
================
*/
void GLMesh_LoadVertexBuffers (void)
{
	int j;
	qmodel_t *m;
	aliashdr_t *hdr;

	for (j = 1; j < MAX_MODELS; j++)
	{
		if (!(m = cl.model_precache[j])) break;
		if (m->type != mod_alias) continue;

		hdr = (aliashdr_t *) Mod_Extradata (m);
		
		GLMesh_LoadVertexBuffer (m, hdr);
	}
}

/*
================
GLMesh_DeleteVertexBuffers

Delete VBOs for all loaded alias models
================
*/
void GLMesh_DeleteVertexBuffers (void)
{
	int j;
	qmodel_t *m;
	
	if (!gl_vbo_able)
		return;
	
	for (j = 1; j < MAX_MODELS; j++)
	{
		if (!(m = cl.model_precache[j])) break;
		if (m->type != mod_alias) continue;

		if (m->meshvbo)
			GL_DeleteBuffersFunc (1, &m->meshvbo);
		m->meshvbo = 0;
		free(m->meshvboptr);
		m->meshvboptr = NULL;

		if (m->meshindexesvbo)
			GL_DeleteBuffersFunc (1, &m->meshindexesvbo);
		m->meshindexesvbo = 0;
		free(m->meshindexesvboptr);
		m->meshindexesvboptr = NULL;
	}

	GL_ClearBufferBindings ();
}






//from gl_model.c
extern char	loadname[];	// for hunk tags
void Mod_CalcAliasBounds (aliashdr_t *a);


#define MD3_VERSION 15
//structures from Tenebrae
typedef struct {
	int			ident;
	int			version;

	char		name[64];

	int			flags;	//assumed to match quake1 models, for lack of somewhere better.

	int			numFrames;
	int			numTags;
	int			numSurfaces;

	int			numSkins;

	int			ofsFrames;
	int			ofsTags;
	int			ofsSurfaces;
	int			ofsEnd;
} md3Header_t;

//then has header->numFrames of these at header->ofs_Frames
typedef struct md3Frame_s {
	vec3_t		bounds[2];
	vec3_t		localOrigin;
	float		radius;
	char		name[16];
} md3Frame_t;

//there are header->numSurfaces of these at header->ofsSurfaces, following from ofsEnd
typedef struct {
	int		ident;				//

	char	name[64];	// polyset name

	int		flags;
	int		numFrames;			// all surfaces in a model should have the same

	int		numShaders;			// all surfaces in a model should have the same
	int		numVerts;

	int		numTriangles;
	int		ofsTriangles;

	int		ofsShaders;			// offset from start of md3Surface_t
	int		ofsSt;				// texture coords are common for all frames
	int		ofsXyzNormals;		// numVerts * numFrames

	int		ofsEnd;				// next surface follows
} md3Surface_t;

//at surf+surf->ofsXyzNormals
/*typedef struct {
	short		xyz[3];
	byte		latlong[2];
} md3XyzNormal_t;*/

//surf->numTriangles at surf+surf->ofsTriangles
typedef struct {
	int			indexes[3];
} md3Triangle_t;

//surf->numVerts at surf+surf->ofsSt
typedef struct {
	float		s;
	float		t;
} md3St_t;

typedef struct {
	char			name[64];
	int				shaderIndex;
} md3Shader_t;



void Mod_LoadMD3Model (qmodel_t *mod, void *buffer)
{
	md3Header_t			*pinheader;
	md3Surface_t		*pinsurface;
	md3Frame_t			*pinframes;
	md3Triangle_t		*pintriangle;
	unsigned short		*poutindexes;
	md3XyzNormal_t		*pinvert;
	md3XyzNormal_t		*poutvert;
	md3St_t				*pinst;
	aliasmesh_t			*poutst;
	md3Shader_t			*pinshader;
	int					size;
	int					start, end, total;
	int					ival, j;
	int					numsurfs, surf;
	int					numframes;
	aliashdr_t			*outhdr;

	start = Hunk_LowMark ();

	pinheader = (md3Header_t *)buffer;

	ival = LittleLong (pinheader->version);
	if (ival != MD3_VERSION)
		Sys_Error ("%s has wrong version number (%i should be %i)",
				 mod->name, ival, MD3_VERSION);

	numsurfs = LittleLong (pinheader->numSurfaces);
	numframes = LittleLong(pinheader->numFrames);

	if (numframes > MAXALIASFRAMES)
		Sys_Error ("%s has too many frames (%i vs %i)",
				 mod->name, numframes, MAXALIASFRAMES);
	if (!numsurfs)
		Sys_Error ("%s has nosurfaces", mod->name);

	pinframes = (md3Frame_t*)((byte*)buffer + LittleLong(pinheader->ofsFrames));
//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
	size	= sizeof(aliashdr_t) + (numframes-1) * sizeof (outhdr->frames[0]);
	outhdr = (aliashdr_t *) Hunk_AllocName (size * numsurfs, loadname);

	for (surf = 0, pinsurface = (md3Surface_t*)((byte*)buffer + LittleLong(pinheader->ofsSurfaces)); surf < numsurfs; surf++, pinsurface = (md3Surface_t*)((byte*)pinsurface + LittleLong(pinsurface->ofsEnd)))
	{
		aliashdr_t	*osurf = (aliashdr_t*)((byte*)outhdr + size*surf);
		if (LittleLong(pinsurface->ident) != (('I'<<0)|('D'<<8)|('P'<<16)|('3'<<24)))
			Sys_Error ("%s corrupt surface ident", mod->name);
		if (LittleLong(pinsurface->numFrames) != numframes)
			Sys_Error ("%s mismatched framecounts", mod->name);

		if (surf+1 < numsurfs)
			osurf->nextsurface = size;
		else
			osurf->nextsurface = 0;
		
		osurf->poseverttype = PV_QUAKE3;
		osurf->numverts_vbo = osurf->numverts = LittleLong(pinsurface->numVerts);
		pinvert = (md3XyzNormal_t*)((byte*)pinsurface + LittleLong(pinsurface->ofsXyzNormals));
		poutvert = (md3XyzNormal_t *) Hunk_Alloc (numframes * osurf->numverts * sizeof(*poutvert));
		osurf->vertexes = (byte *)poutvert - (byte *)osurf;
		for (ival = 0; ival < numframes; ival++)
		{
			osurf->frames[ival].firstpose = ival;
			osurf->frames[ival].numposes = 1;
			osurf->frames[ival].interval = 0.1;
			osurf->frames[ival].frame = ival;

			q_strlcpy(osurf->frames[ival].name, pinframes->name, sizeof(osurf->frames[ival].name));
			for (j = 0; j < 3; j++)
			{	//fixme...
				osurf->frames[ival].bboxmin.v[j] = 0;
				osurf->frames[ival].bboxmax.v[j] = 255;
			}

			for (j=0 ; j<osurf->numverts ; j++)
				poutvert[j] = pinvert[j];
			poutvert += osurf->numverts;
			pinvert += osurf->numverts;
		}
		osurf->numposes = osurf->numframes = numframes;

		osurf->numtris = LittleLong(pinsurface->numTriangles);
		osurf->numindexes = osurf->numtris*3;
		pintriangle = (md3Triangle_t*)((byte*)pinsurface + LittleLong(pinsurface->ofsTriangles));
		poutindexes = (unsigned short *) Hunk_Alloc (sizeof (*poutindexes) * osurf->numindexes);
		osurf->indexes = (intptr_t) poutindexes - (intptr_t) osurf;
		for (ival = 0; ival < osurf->numtris; ival++, pintriangle++, poutindexes+=3)
		{
			for (j = 0; j < 3; j++)
				poutindexes[j] = LittleLong(pintriangle->indexes[j]);
		}

		for (j = 0; j < 3; j++)
		{
			osurf->scale_origin[j] = 0;
			osurf->scale[j] = 1/64.0;
		}

		//guess at skin sizes
		osurf->skinwidth = 320;
		osurf->skinheight = 200;

		//load the textures
		pinshader = (md3Shader_t*)((byte*)pinsurface + LittleLong(pinsurface->ofsShaders));
		osurf->numskins = LittleLong(pinsurface->numShaders);
		for (j = 0; j < osurf->numskins; j++, pinshader++)
		{
			char texturename[MAX_QPATH];
			char fullbrightname[MAX_QPATH];
			char *ext;
			//texture names in md3s are kinda fucked. they could be just names relative to the mdl, or full paths, or just simple shader names.
			//our texture manager is too lame to scan all 1000 possibilities
			if (strchr(pinshader->name, '/') || strchr(pinshader->name, '\\'))
			{	//so if there's a path then we want to use that.
				q_strlcpy(texturename, pinshader->name, sizeof(texturename));
			}
			else
			{	//and if there's no path then we want to prefix it with our own.
				q_strlcpy(texturename, mod->name, sizeof(texturename));
				*(char*)COM_SkipPath(texturename) = 0;
				//and concat the specified name
				q_strlcat(texturename, pinshader->name, sizeof(texturename));
			}
			//and make sure there's no extensions. these get ignored in q3, which is kinda annoying, but this is an md3 and standards are standards (and it makes luma easier).
			ext = (char*)COM_FileGetExtension(texturename);
			if (*ext)
				*--ext = 0;
			//luma has an extra postfix.
			q_snprintf(fullbrightname, sizeof(fullbrightname), "%s_luma", texturename);
			osurf->gltextures[j][0] = TexMgr_LoadImage(mod, texturename, osurf->skinwidth, osurf->skinheight, SRC_EXTERNAL, NULL, texturename, 0, TEXPREF_PAD|TEXPREF_ALPHA|TEXPREF_NOBRIGHT|TEXPREF_MIPMAP);
			osurf->fbtextures[j][0] = TexMgr_LoadImage(mod, fullbrightname, osurf->skinwidth, osurf->skinheight, SRC_EXTERNAL, NULL, texturename, 0, TEXPREF_PAD|TEXPREF_ALPHA|TEXPREF_FULLBRIGHT|TEXPREF_MIPMAP);
			osurf->gltextures[j][3] = osurf->gltextures[j][2] = osurf->gltextures[j][1] = osurf->gltextures[j][0];
			osurf->fbtextures[j][3] = osurf->fbtextures[j][2] = osurf->fbtextures[j][1] = osurf->fbtextures[j][0];
		}
		if (osurf->numskins)
		{
			osurf->skinwidth = osurf->gltextures[0][0]->source_width;
			osurf->skinheight = osurf->gltextures[0][0]->source_height;
		}

		//and figure out the texture coords properly, now we know the actual sizes.
		pinst = (md3St_t*)((byte*)pinsurface + LittleLong(pinsurface->ofsSt));
		poutst = (aliasmesh_t *) Hunk_Alloc (sizeof (*poutst) * osurf->numverts);
		osurf->meshdesc = (intptr_t) poutst - (intptr_t) osurf;
		for (j = 0; j < osurf->numverts; j++)
		{
			poutst[j].vertindex = j;	//how is this useful?
			poutst[j].st[0] = pinst->s;
			poutst[j].st[1] = pinst->t;
		}
	}
	GLMesh_LoadVertexBuffer (mod, outhdr);

	//small violation of the spec, but it seems like noone else uses it.
	mod->flags = LittleLong (pinheader->flags);


	mod->type = mod_alias;

	Mod_CalcAliasBounds (outhdr); //johnfitz

//
// move the complete, relocatable alias model to the cache
//
	end = Hunk_LowMark ();
	total = end - start;

	Cache_Alloc (&mod->cache, total, loadname);
	if (!mod->cache.data)
		return;
	memcpy (mod->cache.data, outhdr, total);

	Hunk_FreeToLowMark (start);
}
