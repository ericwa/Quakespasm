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

#version 110

// Note: we’re not loading this directly but first compiling it to ARB_vertex_program
// using compile_vertexshader.sh

uniform float Blend;
uniform vec3 ShadeVector;
uniform vec4 LightColor;
attribute vec4 Pose1Vert;
attribute vec3 Pose1Normal;
attribute vec4 Pose2Vert;
attribute vec3 Pose2Normal;

// r_avertexnormal_dot() produces the same result as the lookup table in r_alias.c
// (r_avertexnormal_dots[quantized entity angle, from 0-15][compressed vertex normal, 0-161])
// but takes an ordinary vec3 vertex normal instead of a 1-byte compressed normal,
// and requires a ShadeVector uniform variable set as follows:
//
//     float radAngle = (quantangle / 16.0) * 2.0 * 3.14159;
//     shadevector[0] = cos(-radAngle);
//     shadevector[1] = sin(-radAngle);
//     shadevector[2] = 1;
//     VectorNormalize(shadevector);
//
// where quantangle is the qunatized entity angle, from 0-15.
//
// Thanks MH for providing this (from: http://forums.inside3d.com/viewtopic.php?p=39361#p39361 )
float r_avertexnormal_dot(vec3 vertexnormal)
{
        float dot = dot(vertexnormal, ShadeVector);
        // wtf - this reproduces anorm_dots within as reasonable a degree of tolerance as the >= 0 case
        if (dot < 0.0)
            return 1.0 + dot * (13.0 / 44.0);
        else
            return 1.0 + dot;
}

void main()
{
// texture coordinates
	gl_TexCoord[0]  = gl_MultiTexCoord0; // regular skin
	gl_TexCoord[1]  = gl_MultiTexCoord0; // fullbright skin (same coordinates as regular)

// lerped vertex
	vec4 lerpedVert = mix(Pose1Vert, Pose2Vert, Blend);
	gl_Position = gl_ModelViewProjectionMatrix * lerpedVert;

// lerped color
	float dot1 = r_avertexnormal_dot(Pose1Normal);
	float dot2 = r_avertexnormal_dot(Pose2Normal);
	gl_FrontColor = LightColor * vec4(vec3(mix(dot1, dot2, Blend)), 1.0);

// fog calculation, from the Orange Book, 2nd ed, section 9.6
	vec3 ecPosition = vec3(gl_ModelViewMatrix * lerpedVert);
	gl_FogFragCoord = abs(ecPosition.z);
};
