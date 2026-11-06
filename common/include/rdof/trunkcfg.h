// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's settings (docs/spec/fw/trunk.md sections 3.8 and 4): the `trunk` app's object, and the manager's
 * provisioning documents (cfg/net, cfg/node/{node}: section 5.7), into the controller's settings and policy
 * (rdof/t3.h). Portable C, no allocation; it needs no Rust (the defaults are kept here beside the controller's own,
 * and a host test checks they match when the controller is built).
 */
#ifndef RDOF_TRUNKCFG_H
#define RDOF_TRUNKCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/jsonp.h"
#include "rdof/mnctl.h"
#include "rdof/t3.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rdof_plan;

/* trunk.md 3.8's defaults, one carrier (rdof_t3_cfg_default's, without the controller), and the open policy. */
void rdof_trunk_cfg_default(rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol);

/* The keys of one object over cfg and pol: those it has, the rest left as they are. Takes the app's keys (system,
   registration, ms, require_registration, refuse_unlisted, private, tg, wide_tg, static_tg, foacsu, timers,
   broadcast) and, for cfg/node, a top-level site and area_bits; carriers, rev, at, neighbors and enabled are the
   caller's. path names the object in errors ("apps.site", "cfg/net"). false with err set ("path.key: why"); cfg
   and pol may then be half applied. The ranges are checked once all keys are in (rdof_trunk_cfg_check). */
bool rdof_trunk_cfg_apply(const rdof_jdoc_t *doc, int obj, const char *path, rdof_t3_cfg_t *cfg,
                          rdof_t3_policy_t *pol, char *err, size_t cap);

/* The settings' ranges (trunk.md 3.8, the model's NET and SITE bits): false with err set. */
bool rdof_trunk_cfg_check(const rdof_t3_cfg_t *cfg, char *err, size_t cap);

/* A plan's trunk unit: the defaults, its app's keys, its carriers (frequencies, chan, the TSCC's first) and the
   site's color code. unit: the trunk unit's index (-1: the plan's one). false with err set: no trunk unit, or a key
   refused. */
bool rdof_trunk_cfg_plan(const struct rdof_plan *p, int unit, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                         size_t cap);

/* The manager's provisioning, resolved (rdof/mnctl.h rdof_mn_cfg_site), over cfg and pol: the values it has; its
   timers by name. Neighbors and enabled are the caller's. false with err set (cfg and pol may be half applied: keep a
   copy). */
bool rdof_trunk_cfg_site(const rdof_mn_site_cfg_t *sc, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                         size_t cap);

/* The plan's trunk unit, -1 none. */
int rdof_trunk_unit(const struct rdof_plan *p);

#ifdef __cplusplus
}
#endif

#endif
