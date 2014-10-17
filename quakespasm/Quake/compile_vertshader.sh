#!/bin/sh

cgc -o vpalias.vp -profile arbvp1 -oglsl vsalias.glsl

rm vpalias.h

while read line
do
  echo "\"$line\\\\n\"" >> vpalias.h
done < vpalias.vp
