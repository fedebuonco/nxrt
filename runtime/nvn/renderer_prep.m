// Prepared-object tables (split from renderer.m).
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>

#include "renderer_prep.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, Prog *> *progs;
static NSMutableDictionary<NSString *, id> *pipes;
static NSMutableDictionary<NSNumber *, NSValue *> *cs;
static _Atomic int pending;

void renderer_prep_init(void) {
    pthread_mutex_lock(&lock);
    if (!progs) {
        progs = [NSMutableDictionary new];
        pipes = [NSMutableDictionary new];
        cs = [NSMutableDictionary new];
    }
    pthread_mutex_unlock(&lock);
}

Prog *renderer_prep_prog(uint64_t key) {
    if (!progs) return nil;
    pthread_mutex_lock(&lock);
    Prog *p = progs[@(key)];
    pthread_mutex_unlock(&lock);
    return p;
}
void renderer_prep_put_prog(uint64_t key, Prog *pr) {
    if (!progs) return;
    pthread_mutex_lock(&lock);
    progs[@(key)] = pr;
    pthread_mutex_unlock(&lock);
}
id renderer_prep_pipe(NSString *key) {
    if (!pipes) return nil;
    pthread_mutex_lock(&lock);
    id p = pipes[key];
    pthread_mutex_unlock(&lock);
    return p;
}
void renderer_prep_put_pipe(NSString *key, id pipe) {
    if (!pipes) return;
    pthread_mutex_lock(&lock);
    pipes[key] = pipe;
    pthread_mutex_unlock(&lock);
}
NSValue *renderer_prep_cs(uint64_t key) {
    if (!cs) return nil;
    pthread_mutex_lock(&lock);
    NSValue *v = cs[@(key)];
    pthread_mutex_unlock(&lock);
    return v;
}
void renderer_prep_put_cs(uint64_t key, NSValue *v) {
    if (!cs) return;
    pthread_mutex_lock(&lock);
    cs[@(key)] = v;
    pthread_mutex_unlock(&lock);
}
int renderer_prep_pending(void) { return pending; }
void renderer_prep_pending_add(int n) { pending += n; }
