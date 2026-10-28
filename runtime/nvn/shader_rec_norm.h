// Canonical form of a ShaderRec: drop state that cannot affect the result, so two equivalent draws/records are one key.
// (The Metal pipeline key already reads colour state only for targets with a format; this makes the record agree.)
#pragma once
#include "shader_prep.h"

void shader_rec_normalize(ShaderRec *r);
