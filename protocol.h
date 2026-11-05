/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  protocol.h - Semantic pass, part 2: conversation protocol checking.
 *
 *  Every endpoint (ch.client / ch.server created by nali, or a sira
 *  parameter of a banda) is a small state machine whose states are the
 *  steps of its baatcheet. protocol_check() walks every kaam and banda body
 *  in program order and simulates those state machines statically.
 *
 *  Rules enforced:
 *    - direction: an endpoint must bolo when its role speaks next and suno
 *      when the other role speaks next
 *    - message type: the value sent / the variable received into must have
 *      the type declared for that step (i64 or thali[N])
 *    - no use after the conversation has finished
 *    - every conversation must be finished at the end of its scope
 *      (end of main / kaam / banda / block) and at every waapis
 *    - every endpoint must be used (or handed to a banda with paida)
 *    - paida hands an endpoint over: it must not have been used before and
 *      cannot be used by the spawner afterwards
 *    - agar/warna: both branches must leave every endpoint in the same state
 *      (an agar without warna must not change any endpoint's state)
 *    - loops: a jab_tak / ghoom body must be protocol-neutral, i.e. it may
 *      not touch an endpoint declared outside the loop. Channels created
 *      inside the loop body are fine as long as they finish in the body.
 */
#ifndef RABTA_PROTOCOL_H
#define RABTA_PROTOCOL_H

#include "ast.h"

/* Run protocol checking over the type-checked program. Returns #errors. */
int protocol_check(Node *program);

#endif /* RABTA_PROTOCOL_H */
