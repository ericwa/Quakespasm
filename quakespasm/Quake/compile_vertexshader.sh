#!/bin/sh

# cgc from https://developer.nvidia.com/cg-toolkit

rm -f r_alias_vertexshader.h
rm -f r_alias_vertexshader.vp

cgc -o r_alias_vertexshader.vp -profile arbvp1 -oglsl r_alias_vertexshader.glsl

while read line
do
  echo "\"$line\\\\n\"" >> r_alias_vertexshader.h
done < r_alias_vertexshader.vp

rm -f r_alias_vertexshader.vp
