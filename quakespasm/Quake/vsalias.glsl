#version 110

uniform float Blend;
uniform vec3 ShadeVector;
uniform vec4 LightColor;
attribute vec4 Pose1Vert;
attribute vec3 Pose1Normal;
attribute vec4 Pose2Vert;
attribute vec3 Pose2Normal;
float r_avertexnormal_dot(vec3 vertexnormal) // from MH 
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
	gl_TexCoord[0]  = gl_MultiTexCoord0;
	gl_TexCoord[1]  = gl_MultiTexCoord0;
	vec4 lerpedVert = mix(Pose1Vert, Pose2Vert, Blend);
	gl_Position = gl_ModelViewProjectionMatrix * lerpedVert;
	float dot1 = r_avertexnormal_dot(Pose1Normal);
	float dot2 = r_avertexnormal_dot(Pose2Normal);
	gl_FrontColor = LightColor * vec4(vec3(mix(dot1, dot2, Blend)), 1.0);
	// fog
	vec3 ecPosition = vec3(gl_ModelViewMatrix * lerpedVert);
	gl_FogFragCoord = abs(ecPosition.z);
};
