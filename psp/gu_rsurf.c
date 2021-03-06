/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// GL_RSURF.C: surface-related refresh code
#include <assert.h>

#include "gu_local.h"

#include <pspkernel.h>

static vec3_t	modelorg;		// relative to viewpoint

msurface_t	*r_alpha_surfaces;

#define DYNAMIC_LIGHT_WIDTH  128
#define DYNAMIC_LIGHT_HEIGHT 128

#define LIGHTMAP_BYTES 4

#define	BLOCK_WIDTH		128
#define	BLOCK_HEIGHT	128

#define	MAX_LIGHTMAPS	20

int		c_visible_lightmaps;
int		c_visible_textures;

typedef struct gllightmapstate_s
{
	int	current_lightmap_texture;

	msurface_t	*lightmap_surfaces[MAX_LIGHTMAPS];

	int			allocated[BLOCK_WIDTH];

	// the lightmap texture data needs to be kept in
	// main memory so texsubimage can update properly
	byte		lightmap_buffer[4*BLOCK_WIDTH*BLOCK_HEIGHT];
} gllightmapstate_t;

static byte __attribute__((aligned(16))) lightmap_textures[MAX_LIGHTMAPS][BLOCK_WIDTH * BLOCK_HEIGHT];

static gllightmapstate_t gl_lms;


static void		LM_InitBlock( void );
static void		LM_UploadBlock( qboolean dynamic );
static qboolean	LM_AllocBlock (int w, int h, int *x, int *y);

extern void R_SetCacheState( msurface_t *surf );
extern void R_BuildLightMap (msurface_t *surf, byte *dest, int stride);

/*
=============================================================

	BRUSH MODELS

=============================================================
*/

/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
image_t *R_TextureAnimation (mtexinfo_t *tex)
{
	int		c;

	if (!tex->next)
		return tex->image;

	c = currententity->frame % tex->numframes;
	while (c)
	{
		tex = tex->next;
		c--;
	}

	return tex->image;
}

#if 0
/*
=================
WaterWarpPolyVerts

Mangles the x and y coordinates in a copy of the poly
so that any drawing routine can be water warped
=================
*/
glpoly_t *WaterWarpPolyVerts (glpoly_t *p)
{
	int		i;
	float	*v, *nv;
	static byte	buffer[1024];
	glpoly_t *out;

	out = (glpoly_t *)buffer;

	out->numverts = p->numverts;
	v = p->verts[0];
	nv = out->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE, nv+=VERTEXSIZE)
	{
		nv[0] = v[0] + 4*sin(v[1]*0.05+r_newrefdef.time)*sin(v[2]*0.05+r_newrefdef.time);
		nv[1] = v[1] + 4*sin(v[0]*0.05+r_newrefdef.time)*sin(v[2]*0.05+r_newrefdef.time);

		nv[2] = v[2];
		nv[3] = v[3];
		nv[4] = v[4];
		nv[5] = v[5];
		nv[6] = v[6];
	}

	return out;
}

/*
================
DrawGLWaterPoly

Warp the vertex coordinates
================
*/
void DrawGLWaterPoly (glpoly_t *p)
{
	int		i;
	float	*v;

	p = WaterWarpPolyVerts (p);
	qglBegin (GL_TRIANGLE_FAN);
	v = p->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE)
	{
		qglTexCoord2f (v[3], v[4]);
		qglVertex3fv (v);
	}
	qglEnd ();
}
void DrawGLWaterPolyLightmap (glpoly_t *p)
{
	int		i;
	float	*v;

	p = WaterWarpPolyVerts (p);
	qglBegin (GL_TRIANGLE_FAN);
	v = p->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE)
	{
		qglTexCoord2f (v[5], v[6]);
		qglVertex3fv (v);
	}
	qglEnd ();
}
#endif

/*
================
DrawGLPoly
================
*/
static void Intersect(float *dst, const float *a, const float *b, float dist_a, float dist_b)
{
	const float f = dist_a / (dist_a - dist_b);
	int i;

	for (i = 0; i < VERTEXSIZE; ++i)
	{
		dst[i] = a[i] + (f * (b[i] - a[i]));
	}
}

static int ClipAgainstPlane(float *dst, const float *src, int numverts, const cplane_t *plane)
{
	const float *const end = &src[numverts * VERTEXSIZE];

	const float *a = &src[0];
	const float *b = &src[(numverts - 1) * VERTEXSIZE];
	int num_clipped_verts = 0;

	while (a != end)
	{
		const float dist_a = DotProduct(a, plane->normal) - plane->dist;
		const float dist_b = DotProduct(b, plane->normal) - plane->dist;
		int i;

		// A inside?
		if (dist_a > 0)
		{
			// B outside?
			if (dist_b <= 0)
			{
				// Add intersection.
				Intersect(dst, a, b, dist_a, dist_b);
				dst += VERTEXSIZE;
				++num_clipped_verts;
			}

			// Add a.
			for (i = 0; i < VERTEXSIZE; ++i)
			{
				*dst++ = a[i];
			}
			++num_clipped_verts;
		}
		else
		{
			// B inside?
			if (dist_b > 0)
			{
				// Add intersection.
				Intersect(dst, a, b, dist_a, dist_b);
				dst += VERTEXSIZE;
				++num_clipped_verts;
			}
		}

		// Move on to the next edge.
		b = a;
		a += VERTEXSIZE;
	}

	return num_clipped_verts;
}

static int ClipAgainstFrustum(float *dst, const float *src, int numverts)
{
	float tmp[R_MAX_CLIPPED_VERTS * VERTEXSIZE];

	numverts = ClipAgainstPlane(tmp, src, numverts, &frustum[0]);
	if (numverts == 0)
	{
		return 0;
	}

	numverts = ClipAgainstPlane(dst, tmp, numverts, &frustum[1]);
	if (numverts == 0)
	{
		return 0;
	}

	numverts = ClipAgainstPlane(tmp, dst, numverts, &frustum[2]);
	if (numverts == 0)
	{
		return 0;
	}

	numverts = ClipAgainstPlane(dst, tmp, numverts, &frustum[3]);
	if (numverts == 0)
	{
		return 0;
	}

	return numverts;
}

static void DrawGLPoly (glpoly_t *p, int cull)
{
	float	clipped[R_MAX_CLIPPED_VERTS * VERTEXSIZE];
	int		numverts;
	const float	*v;
	glvert_t *out;
	int		i;

	if (cull == 3)
	{
		numverts = ClipAgainstFrustum(clipped, p->verts[0], p->numverts);
		if (numverts == 0)
		{
			return;
		}
		v = clipped;
	}
	else
	{
		numverts = p->numverts;
		v = p->verts[0];
	}

	// Allocate vertices.
	out = sceGuGetMemory(numverts * sizeof(glvert_t));

	// Fill vertices.
	for (i = 0; i < numverts; ++i)
	{
		glvert_t *const dst = &out[i];

		dst->x = *v++;
		dst->y = *v++;
		dst->z = *v++;
		dst->s = *v++;
		dst->t = *v;

		v += (VERTEXSIZE - 4);
	}

	// Draw.
	sceGumDrawArray(GU_TRIANGLE_FAN, GLVERT_TYPE, numverts, NULL, out);
}

//============
//PGM
/*
================
DrawGLFlowingPoly -- version of DrawGLPoly that handles scrolling texture
================
*/
void DrawGLFlowingPoly (msurface_t *fa)
{
#ifndef PSP
	int		i;
	float	*v;
	glpoly_t *p;
	float	scroll;

	p = fa->polys;

	scroll = -64 * ( (r_newrefdef.time / 40.0) - (int)(r_newrefdef.time / 40.0) );
	if(scroll == 0.0)
		scroll = -64.0;

	qglBegin (GL_POLYGON);
	v = p->verts[0];
	for (i=0 ; i<p->numverts ; i++, v+= VERTEXSIZE)
	{
		qglTexCoord2f ((v[3] + scroll), v[4]);
		qglVertex3fv (v);
	}
	qglEnd ();
#endif
}
//PGM
//============

/*
** R_DrawTriangleOutlines
*/
void R_DrawTriangleOutlines (void)
{
#ifndef PSP
	int			i, j;
	glpoly_t	*p;

	if (!gl_showtris->value)
		return;

	qglDisable (GL_TEXTURE_2D);
	qglDisable (GL_DEPTH_TEST);
	qglColor4f (1,1,1,1);

	for (i=0 ; i<MAX_LIGHTMAPS ; i++)
	{
		msurface_t *surf;

		for ( surf = gl_lms.lightmap_surfaces[i]; surf != 0; surf = surf->lightmapchain )
		{
			p = surf->polys;
			for (j=2 ; j<p->numverts ; j++ )
			{
				qglBegin (GL_LINE_STRIP);
				qglVertex3fv (p->verts[0]);
				qglVertex3fv (p->verts[j-1]);
				qglVertex3fv (p->verts[j]);
				qglVertex3fv (p->verts[0]);
				qglEnd ();
			}
		}
	}

	qglEnable (GL_DEPTH_TEST);
	qglEnable (GL_TEXTURE_2D);
#endif
}

/*
** DrawGLPolyChain
*/
static void DrawGLPolyChain( glpoly_t *p, float soffset, float toffset, int cull )
{
	float	clipped[R_MAX_CLIPPED_VERTS * VERTEXSIZE];
	int		numverts;
	const float	*v;
	glvert_t *out;
	int		i;

	if (cull == 3)
	{
		numverts = ClipAgainstFrustum(clipped, p->verts[0], p->numverts);
		if (numverts == 0)
		{
			return;
		}
		v = clipped;
	}
	else
	{
		numverts = p->numverts;
		v = p->verts[0];
	}

	// Allocate vertices.
	out = sceGuGetMemory(numverts * sizeof(glvert_t));

	// Fill vertices.
	for (i = 0; i < numverts; ++i)
	{
		glvert_t *const dst = &out[i];

		dst->x = *v++;
		dst->y = *v++;
		dst->z = *v;

		v += 3;

		dst->s = *v++ - soffset;
		dst->t = *v++ - toffset;
	}

	// Draw.
	sceGumDrawArray(GU_TRIANGLE_FAN, GLVERT_TYPE, numverts, NULL, out);
}

/*
** R_BlendLightMaps
**
** This routine takes all the given light mapped surfaces in the world and
** blends them into the framebuffer.
*/
static void R_BlendLightmaps (void)
{
	int			i;
	msurface_t	*surf, *newdrawsurf = 0;

	// don't bother if we're set to fullbright
	if (r_fullbright->value)
		return;
	if (!r_worldmodel->lightdata)
		return;

	// don't bother writing Z
	sceGuDepthMask(GU_TRUE);

	/*
	** set the appropriate blending mode unless we're only looking at the
	** lightmaps.
	*/
	if (!gl_lightmap->value)
	{
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0); // Add and multiply by 2. Maybe there's a more efficient way.
	}

	if ( currentmodel == r_worldmodel )
		c_visible_lightmaps = 0;

	/*
	** render static lightmaps first
	*/
	for ( i = 1; i < MAX_LIGHTMAPS; i++ )
	{
		if ( gl_lms.lightmap_surfaces[i] )
		{
			if (currentmodel == r_worldmodel)
				c_visible_lightmaps++;

			sceGuTexImage(0, BLOCK_WIDTH, BLOCK_HEIGHT, BLOCK_WIDTH, &lightmap_textures[i][0]);

			for ( surf = gl_lms.lightmap_surfaces[i]; surf != 0; surf = surf->lightmapchain )
			{
				if ( surf->polys )
					DrawGLPolyChain( surf->polys, 0, 0, surf->cull );
			}
		}
	}

	/*
	** render dynamic lightmaps
	*/
	if ( gl_dynamic->value )
	{
		LM_InitBlock();

		if (currentmodel == r_worldmodel)
			c_visible_lightmaps++;

		newdrawsurf = gl_lms.lightmap_surfaces[0];

		sceGuTexImage(0, BLOCK_WIDTH, BLOCK_HEIGHT, BLOCK_WIDTH, &lightmap_textures[0][0]);

		for ( surf = gl_lms.lightmap_surfaces[0]; surf != 0; surf = surf->lightmapchain )
		{
			int		smax, tmax;
			byte	*base;

			smax = (surf->extents[0]>>4)+1;
			tmax = (surf->extents[1]>>4)+1;

			if ( LM_AllocBlock( smax, tmax, &surf->dlight_s, &surf->dlight_t ) )
			{
				base = gl_lms.lightmap_buffer;
				base += ( surf->dlight_t * BLOCK_WIDTH + surf->dlight_s ) * LIGHTMAP_BYTES;

				R_BuildLightMap (surf, base, BLOCK_WIDTH*LIGHTMAP_BYTES);
			}
			else
			{
				msurface_t *drawsurf;

				// Finish drawing.
				// TODO Is this right?
				sceGuSync(GU_SYNC_DONE, GU_SYNC_WHAT_DONE);

				// upload what we have so far
				LM_UploadBlock( true );

				// Flush texture cache.
				sceKernelDcacheWritebackRange(&lightmap_textures[0], sizeof(lightmap_textures[0]));
				sceGuTexFlush();

				// draw all surfaces that use this lightmap
				for ( drawsurf = newdrawsurf; drawsurf != surf; drawsurf = drawsurf->lightmapchain )
				{
					if ( drawsurf->polys )
					{
						DrawGLPolyChain(
							drawsurf->polys,
							( drawsurf->light_s - drawsurf->dlight_s ) * ( 1.0 / 128.0 ),
							( drawsurf->light_t - drawsurf->dlight_t ) * ( 1.0 / 128.0 ),
							surf->cull);
					}
				}

				newdrawsurf = drawsurf;

				// clear the block
				LM_InitBlock();

				// try uploading the block now
				if ( !LM_AllocBlock( smax, tmax, &surf->dlight_s, &surf->dlight_t ) )
				{
					ri.Sys_Error( ERR_FATAL, "Consecutive calls to LM_AllocBlock(%d,%d) failed (dynamic)\n", smax, tmax );
				}

				base = gl_lms.lightmap_buffer;
				base += ( surf->dlight_t * BLOCK_WIDTH + surf->dlight_s ) * LIGHTMAP_BYTES;

				R_BuildLightMap (surf, base, BLOCK_WIDTH*LIGHTMAP_BYTES);
			}
		}

		/*
		** draw remainder of dynamic lightmaps that haven't been uploaded yet
		*/
		if (newdrawsurf)
		{
			LM_UploadBlock(true);

			// Flush texture cache.
			sceKernelDcacheWritebackRange(&lightmap_textures[0], sizeof(lightmap_textures[0]));
			sceGuTexFlush();
		}

		for ( surf = newdrawsurf; surf != 0; surf = surf->lightmapchain )
		{
			if ( surf->polys )
			{
				DrawGLPolyChain(
					surf->polys,
					( surf->light_s - surf->dlight_s ) * ( 1.0 / 128.0 ),
					( surf->light_t - surf->dlight_t ) * ( 1.0 / 128.0 ),
					surf->cull);
			}
		}
	}

	/*
	** restore state
	*/
	sceGuDisable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuDepthMask(GU_FALSE);
}

/*
================
R_RenderBrushPoly
================
*/
void R_RenderBrushPoly (msurface_t *fa)
{
	int			maps;
	image_t		*image;
	qboolean is_dynamic = false;

	c_brush_polys++;

	image = R_TextureAnimation (fa->texinfo);

	if (fa->flags & SURF_DRAWTURB)
	{	
#ifndef PSP
		GL_Bind( image->texnum );

		// warp texture, no lightmaps
		GL_TexEnv( GU_TFX_MODULATE );
		qglColor4f( gl_state.inverse_intensity, 
			        gl_state.inverse_intensity,
					gl_state.inverse_intensity,
					1.0F );
		EmitWaterPolys (fa);
		GL_TexEnv( GU_TFX_REPLACE );
#endif

		return;
	}
	else
	{
		GL_Bind( image );
#ifndef PSP
		GL_TexEnv( GU_TFX_REPLACE );
#endif
	}

//======
//PGM
	if(fa->texinfo->flags & SURF_FLOWING)
		DrawGLFlowingPoly (fa);
	else
		DrawGLPoly (fa->polys, fa->cull);
//PGM
//======

	/*
	** check for lightmap modification
	*/
	for ( maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++ )
	{
		if ( r_newrefdef.lightstyles[fa->styles[maps]].white != fa->cached_light[maps] )
			goto dynamic;
	}

	// dynamic this frame or dynamic previously
	if ( ( fa->dlightframe == r_framecount ) )
	{
dynamic:
		if ( gl_dynamic->value )
		{
			if (!( fa->texinfo->flags & (SURF_SKY|SURF_TRANS33|SURF_TRANS66|SURF_WARP ) ) )
			{
				is_dynamic = true;
			}
		}
	}

	if ( is_dynamic )
	{
		if ( ( fa->styles[maps] >= 32 || fa->styles[maps] == 0 ) && ( fa->dlightframe != r_framecount ) )
		{
			unsigned	temp[34*34];
			int			smax, tmax;

			smax = (fa->extents[0]>>4)+1;
			tmax = (fa->extents[1]>>4)+1;

			R_BuildLightMap( fa, (void *)temp, smax*4 );
			R_SetCacheState( fa );

#ifndef PSP
			GL_Bind( gl_state.lightmap_textures + fa->lightmaptexturenum );

			qglTexSubImage2D( GL_TEXTURE_2D, 0,
							  fa->light_s, fa->light_t, 
							  smax, tmax, 
							  GL_LIGHTMAP_FORMAT, 
							  GL_UNSIGNED_BYTE, temp );
#endif

			fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
			gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
		}
		else
		{
			fa->lightmapchain = gl_lms.lightmap_surfaces[0];
			gl_lms.lightmap_surfaces[0] = fa;
		}
	}
	else
	{
		fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
		gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
	}
}


/*
================
R_DrawAlphaSurfaces

Draw water surfaces and windows.
The BSP tree is waled front to back, so unwinding the chain
of alpha_surfaces will draw back to front, giving proper ordering.
================
*/
void R_DrawAlphaSurfaces (void)
{
	msurface_t	*s;
	float		intens;

	ASSERT(GU_IsInDisplayList());

	//
	// go back to the world matrix
	//
	sceGumLoadIdentity();

	// the textures are prescaled up for a better lighting range,
	// so scale it back down
	intens = gl_state.inverse_intensity;

	for (s=r_alpha_surfaces ; s ; s=s->texturechain)
	{
		GL_Bind(s->texinfo->image);
		c_brush_polys++;
#ifndef PSP
		if (s->texinfo->flags & SURF_TRANS33)
			qglColor4f (intens,intens,intens,0.33);
		else if (s->texinfo->flags & SURF_TRANS66)
			qglColor4f (intens,intens,intens,0.66);
		else
			qglColor4f (intens,intens,intens,1);
#endif
		if (s->flags & SURF_DRAWTURB)
			EmitWaterPolys (s);
		else if(s->texinfo->flags & SURF_FLOWING)			// PGM	9/16/98
			DrawGLFlowingPoly (s);							// PGM
		else
			DrawGLPoly (s->polys, s->cull);
	}

#ifndef PSP
	GL_TexEnv( GU_TFX_REPLACE );
	qglColor4f (1,1,1,1);
	qglDisable (GU_BLEND);
#endif

	r_alpha_surfaces = NULL;
}

/*
================
DrawTextureChains
================
*/
void DrawTextureChains (void)
{
	int		i;
	msurface_t	*s;
	image_t		*image;

	c_visible_textures = 0;

//	GL_TexEnv( GL_REPLACE );

	for ( i = 0, image=gltextures ; i<numgltextures ; i++,image++)
	{
		s = image->texturechain;
		if (!s)
			continue;
		c_visible_textures++;

		for ( ; s ; s=s->texturechain)
			R_RenderBrushPoly (s);

		image->texturechain = NULL;
	}

#ifndef PSP
	GL_TexEnv( GU_TFX_REPLACE );
#endif
}

/*
=================
R_DrawInlineBModel
=================
*/
void R_DrawInlineBModel (void)
{
	int			i, k;
	cplane_t	*pplane;
	float		dot;
	msurface_t	*psurf;
	dlight_t	*lt;

	// calculate dynamic lighting for bmodel
	if ( !gl_flashblend->value )
	{
		lt = r_newrefdef.dlights;
		for (k=0 ; k<r_newrefdef.num_dlights ; k++, lt++)
		{
			R_MarkLights (lt, 1<<k, currentmodel->nodes + currentmodel->firstnode);
		}
	}

	psurf = &currentmodel->surfaces[currentmodel->firstmodelsurface];

	if ( currententity->flags & RF_TRANSLUCENT )
	{
#ifndef PSP
		qglEnable (GU_BLEND);
		qglColor4f (1,1,1,0.25);
		GL_TexEnv( GU_TFX_MODULATE );
#endif
	}

	//
	// draw texture
	//
	for (i=0 ; i<currentmodel->nummodelsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			if (psurf->texinfo->flags & (SURF_TRANS33|SURF_TRANS66) )
			{	// add to the translucent chain
				psurf->texturechain = r_alpha_surfaces;
				r_alpha_surfaces = psurf;
			}
			else
			{
				R_RenderBrushPoly( psurf );
			}
		}
	}

	if ( !(currententity->flags & RF_TRANSLUCENT) )
	{
		R_BlendLightmaps ();
	}
	else
	{
#ifndef PSP
		qglDisable (GU_BLEND);
		qglColor4f (1,1,1,1);
		GL_TexEnv( GU_TFX_REPLACE );
#endif
	}
}

/*
=================
R_DrawBrushModel
=================
*/
void R_DrawBrushModel (entity_t *e)
{
	vec3_t		mins, maxs;
	int			i;
	qboolean	rotated;

	if (currentmodel->nummodelsurfaces == 0)
		return;

	currententity = e;
	gl_state.currenttexture = NULL;

	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		rotated = true;
		for (i=0 ; i<3 ; i++)
		{
			mins[i] = e->origin[i] - currentmodel->radius;
			maxs[i] = e->origin[i] + currentmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (e->origin, currentmodel->mins, mins);
		VectorAdd (e->origin, currentmodel->maxs, maxs);
	}

	if (R_CullBox (mins, maxs) == 2)
		return;

#ifndef PSP
	qglColor3f (1,1,1);
#endif
	memset (gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));

	VectorSubtract (r_newrefdef.vieworg, e->origin, modelorg);
	if (rotated)
	{
		vec3_t	temp;
		vec3_t	forward, right, up;

		VectorCopy (modelorg, temp);
		AngleVectors (e->angles, forward, right, up);
		modelorg[0] = DotProduct (temp, forward);
		modelorg[1] = -DotProduct (temp, right);
		modelorg[2] = DotProduct (temp, up);
	}


	sceGumPushMatrix();
e->angles[0] = -e->angles[0];	// stupid quake bug
e->angles[2] = -e->angles[2];	// stupid quake bug
	R_RotateForEntity (e);
e->angles[0] = -e->angles[0];	// stupid quake bug
e->angles[2] = -e->angles[2];	// stupid quake bug

#ifndef PSP
	GL_TexEnv( GU_TFX_REPLACE );
	GL_TexEnv( GU_TFX_MODULATE );
#endif

	R_DrawInlineBModel ();

	sceGumPopMatrix();
}

/*
=============================================================

	WORLD MODEL

=============================================================
*/

/*
================
R_RecursiveWorldNode
================
*/
static void R_RecursiveWorldNode (mnode_t *node, int cull)
{
	int			c, side, sidebit;
	cplane_t	*plane;
	msurface_t	*surf, **mark;
	mleaf_t		*pleaf;
	float		dot;
	image_t		*image;

	if (node->contents == CONTENTS_SOLID)
		return;		// solid

	if (node->visframe != r_visframecount)
		return;
	if (cull != 1)
	{
		cull = R_CullBox (node->minmaxs, node->minmaxs+3);
		if (cull == 2)
			return;
	}
	
// if a leaf node, draw stuff
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;

		// check for door connected areas
		if (r_newrefdef.areabits)
		{
			if (! (r_newrefdef.areabits[pleaf->area>>3] & (1<<(pleaf->area&7)) ) )
				return;		// not visible
		}

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			} while (--c);
		}

		return;
	}

// node is just a decision point, so go down the apropriate sides

// find which side of the node we are on
	plane = node->plane;

	dot = DotProduct (modelorg, plane->normal) - plane->dist;

	if (dot >= 0)
	{
		side = 0;
		sidebit = 0;
	}
	else
	{
		side = 1;
		sidebit = SURF_PLANEBACK;
	}

// recurse down the children, front side first
	R_RecursiveWorldNode (node->children[side], cull);

	// draw stuff
	for ( c = node->numsurfaces, surf = r_worldmodel->surfaces + node->firstsurface; c ; c--, surf++)
	{
		if (surf->visframe != r_framecount)
			continue;

		if ( (surf->flags & SURF_PLANEBACK) != sidebit )
			continue;		// wrong side

		surf->cull = cull;

		if (surf->texinfo->flags & SURF_SKY)
		{	// just adds to visible sky bounds
			R_AddSkySurface (surf);
		}
		else if (surf->texinfo->flags & (SURF_TRANS33|SURF_TRANS66))
		{	// add to the translucent chain
			surf->texturechain = r_alpha_surfaces;
			r_alpha_surfaces = surf;
		}
		else
		{
			// the polygon is visible, so add it to the texture
			// sorted chain
			// FIXME: this is a hack for animation
			image = R_TextureAnimation (surf->texinfo);
			surf->texturechain = image->texturechain;
			image->texturechain = surf;
		}
	}

	// recurse down the back side
	R_RecursiveWorldNode (node->children[!side], cull);
/*
	for ( ; c ; c--, surf++)
	{
		if (surf->visframe != r_framecount)
			continue;

		if ( (surf->flags & SURF_PLANEBACK) != sidebit )
			continue;		// wrong side

		if (surf->texinfo->flags & SURF_SKY)
		{	// just adds to visible sky bounds
			R_AddSkySurface (surf);
		}
		else if (surf->texinfo->flags & (SURF_TRANS33|SURF_TRANS66))
		{	// add to the translucent chain
//			surf->texturechain = alpha_surfaces;
//			alpha_surfaces = surf;
		}
		else
		{
			if ( qglMTexCoord2fSGIS && !( surf->flags & SURF_DRAWTURB ) )
			{
				GL_RenderLightmappedPoly( surf );
			}
			else
			{
				// the polygon is visible, so add it to the texture
				// sorted chain
				// FIXME: this is a hack for animation
				image = R_TextureAnimation (surf->texinfo);
				surf->texturechain = image->texturechain;
				image->texturechain = surf;
			}
		}
	}
*/
}


/*
=============
R_DrawWorld
=============
*/
void R_DrawWorld (void)
{
	entity_t	ent;

	if (!r_drawworld->value)
		return;

	if ( r_newrefdef.rdflags & RDF_NOWORLDMODEL )
		return;

	currentmodel = r_worldmodel;

	VectorCopy (r_newrefdef.vieworg, modelorg);

	// auto cycle the world frame for texture animation
	memset (&ent, 0, sizeof(ent));
	ent.frame = (int)(r_newrefdef.time*2);
	currententity = &ent;

	gl_state.currenttexture = NULL;

#ifndef PSP
	qglColor3f (1,1,1);
#endif
	memset (gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));
	R_ClearSkyBox ();

	R_RecursiveWorldNode (r_worldmodel->nodes, 3);

	/*
	** theoretically nothing should happen in the next two functions
	** if multitexture is enabled
	*/
	DrawTextureChains ();
	R_BlendLightmaps ();
	
	R_DrawSkyBox ();

	R_DrawTriangleOutlines ();
}


/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current
cluster
===============
*/
void R_MarkLeaves (void)
{
	byte	*vis;
	byte	fatvis[MAX_MAP_LEAFS/8];
	mnode_t	*node;
	int		i, c;
	mleaf_t	*leaf;
	int		cluster;

	if (r_oldviewcluster == r_viewcluster && r_oldviewcluster2 == r_viewcluster2 && !r_novis->value && r_viewcluster != -1)
		return;

	// development aid to let you run around and see exactly where
	// the pvs ends
	if (gl_lockpvs->value)
		return;

	r_visframecount++;
	r_oldviewcluster = r_viewcluster;
	r_oldviewcluster2 = r_viewcluster2;

	if (r_novis->value || r_viewcluster == -1 || !r_worldmodel->vis)
	{
		// mark everything
		for (i=0 ; i<r_worldmodel->numleafs ; i++)
			r_worldmodel->leafs[i].visframe = r_visframecount;
		for (i=0 ; i<r_worldmodel->numnodes ; i++)
			r_worldmodel->nodes[i].visframe = r_visframecount;
		return;
	}

	vis = Mod_ClusterPVS (r_viewcluster, r_worldmodel);
	// may have to combine two clusters because of solid water boundaries
	if (r_viewcluster2 != r_viewcluster)
	{
		memcpy (fatvis, vis, (r_worldmodel->numleafs+7)/8);
		vis = Mod_ClusterPVS (r_viewcluster2, r_worldmodel);
		c = (r_worldmodel->numleafs+31)/32;
		for (i=0 ; i<c ; i++)
			((int *)fatvis)[i] |= ((int *)vis)[i];
		vis = fatvis;
	}
	
	for (i=0,leaf=r_worldmodel->leafs ; i<r_worldmodel->numleafs ; i++, leaf++)
	{
		cluster = leaf->cluster;
		if (cluster == -1)
			continue;
		if (vis[cluster>>3] & (1<<(cluster&7)))
		{
			node = (mnode_t *)leaf;
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}

#if 0
	for (i=0 ; i<r_worldmodel->vis->numclusters ; i++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			node = (mnode_t *)&r_worldmodel->leafs[i];	// FIXME: cluster
			do
			{
				if (node->visframe == r_visframecount)
					break;
				node->visframe = r_visframecount;
				node = node->parent;
			} while (node);
		}
	}
#endif
}



/*
=============================================================================

  LIGHTMAP ALLOCATION

=============================================================================
*/

static void LM_InitBlock( void )
{
	memset( gl_lms.allocated, 0, sizeof( gl_lms.allocated ) );
}

static void LM_ConvertToGUFormat(byte *dst, int height)
{
	const byte *const dst_end = dst + (BLOCK_WIDTH * height);
	const byte *src = &gl_lms.lightmap_buffer[0];
	int src_y;

	while (dst != dst_end)
	{
		unsigned int r, g, b, rgb;

		// Read RGB.
		r = *src++;
		g = *src++;
		b = *src;
		src += 2;

		// Calculate RGB.
		rgb = (r >> 3) | ((g >> 2) << 5) | ((g >> 3) << 11);

		// Write index.
		*dst++ = gl_state.d_16to8table[rgb];
	}
}

static void LM_UploadBlock( qboolean dynamic )
{
	int texture;
	int height = 0;

	if ( dynamic )
	{
		texture = 0;
	}
	else
	{
		texture = gl_lms.current_lightmap_texture;
	}

	if ( dynamic )
	{
		int i;

		for ( i = 0; i < BLOCK_WIDTH; i++ )
		{
			if ( gl_lms.allocated[i] > height )
				height = gl_lms.allocated[i];
		}

		LM_ConvertToGUFormat(&lightmap_textures[texture][0], height);
	}
	else
	{
		LM_ConvertToGUFormat(&lightmap_textures[texture][0], BLOCK_HEIGHT);

		if ( ++gl_lms.current_lightmap_texture == MAX_LIGHTMAPS )
			ri.Sys_Error( ERR_DROP, "LM_UploadBlock() - MAX_LIGHTMAPS exceeded\n" );
	}
}

// returns a texture number and the position inside it
static qboolean LM_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;

	best = BLOCK_HEIGHT;

	for (i=0 ; i<BLOCK_WIDTH-w ; i++)
	{
		best2 = 0;

		for (j=0 ; j<w ; j++)
		{
			if (gl_lms.allocated[i+j] >= best)
				break;
			if (gl_lms.allocated[i+j] > best2)
				best2 = gl_lms.allocated[i+j];
		}
		if (j == w)
		{	// this is a valid spot
			*x = i;
			*y = best = best2;
		}
	}

	if (best + h > BLOCK_HEIGHT)
		return false;

	for (i=0 ; i<w ; i++)
		gl_lms.allocated[*x + i] = best + h;

	return true;
}

/*
================
GL_BuildPolygonFromSurface
================
*/
void GL_BuildPolygonFromSurface(msurface_t *fa)
{
	int			i, lindex, lnumverts;
	dedge_t		*pedges, *r_pedge;
	int			vertpage;
	float		*vec;
	float		s, t;
	glpoly_t	*poly;

// reconstruct the polygon
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;
	vertpage = 0;
	
	if (lnumverts > R_MAX_BSP_FACE_VERTS)
	{
		ri.Sys_Error(ERR_DROP, "%s: Poly has too many vertices for clipping. %d > %d", __FUNCTION__, lnumverts, R_MAX_BSP_FACE_VERTS);
	}

	//
	// draw texture
	//
	poly = Hunk_Alloc (&hunk_ref, sizeof(glpoly_t) + (lnumverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = fa->polys;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = currentmodel->vertexes[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = currentmodel->vertexes[r_pedge->v[1]].position;
		}
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->image->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->image->height;

		VectorCopy (vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		//
		// lightmap texture coordinates
		//
		s = DotProduct (vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s*16;
		s += 8;
		s /= BLOCK_WIDTH*16; //fa->texinfo->texture->width;

		t = DotProduct (vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t*16;
		t += 8;
		t /= BLOCK_HEIGHT*16; //fa->texinfo->texture->height;

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}
}

/*
========================
GL_CreateSurfaceLightmap
========================
*/
void GL_CreateSurfaceLightmap (msurface_t *surf)
{
	int		smax, tmax;
	byte	*base;

	if (surf->flags & (SURF_DRAWSKY|SURF_DRAWTURB))
		return;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;

	if ( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ) )
	{
		LM_UploadBlock( false );
		LM_InitBlock();
		if ( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ) )
		{
			ri.Sys_Error( ERR_FATAL, "Consecutive calls to LM_AllocBlock(%d,%d) failed\n", smax, tmax );
		}
	}

	surf->lightmaptexturenum = gl_lms.current_lightmap_texture;

	base = gl_lms.lightmap_buffer;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;

	R_SetCacheState( surf );
	R_BuildLightMap (surf, base, BLOCK_WIDTH*LIGHTMAP_BYTES);
}


/*
==================
GL_BeginBuildingLightmaps

==================
*/
void GL_BeginBuildingLightmaps (model_t *m)
{
	static lightstyle_t	lightstyles[MAX_LIGHTSTYLES];
	int				i;
	unsigned		dummy[128*128];

	memset( gl_lms.allocated, 0, sizeof(gl_lms.allocated) );

	r_framecount = 1;		// no dlightcache

	/*
	** setup the base lightstyles so the lightmaps won't have to be regenerated
	** the first time they're seen
	*/
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		lightstyles[i].rgb[0] = 1;
		lightstyles[i].rgb[1] = 1;
		lightstyles[i].rgb[2] = 1;
		lightstyles[i].white = 3;
	}
	r_newrefdef.lightstyles = lightstyles;

	gl_lms.current_lightmap_texture = 1;
}

/*
=======================
GL_EndBuildingLightmaps
=======================
*/
void GL_EndBuildingLightmaps (void)
{
	LM_UploadBlock( false );
}

