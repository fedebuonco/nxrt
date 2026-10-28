// Prepared-object tables (renderer_prep.m): programs, pipelines and compute programs built before the game.
#pragma once
#import <Foundation/Foundation.h>
#include <stdint.h>
#include "renderer_prog.h"

void renderer_prep_init(void);
Prog *renderer_prep_prog(uint64_t key);
void renderer_prep_put_prog(uint64_t key, Prog *pr);
id renderer_prep_pipe(NSString *key);
void renderer_prep_put_pipe(NSString *key, id pipe);
NSValue *renderer_prep_cs(uint64_t key);
void renderer_prep_put_cs(uint64_t key, NSValue *v);
int renderer_prep_pending(void);
void renderer_prep_pending_add(int n);
