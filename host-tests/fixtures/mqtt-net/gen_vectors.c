// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Prints vectors.json: the MQTT network's call messages (docs/spec/fw/mqtt-net.md section 13), decoded.
 *
 *   rdof_gen_mqttnet_vectors > host-tests/fixtures/mqtt-net/vectors.json
 *
 * "vectors": single messages made with the writer: section 13's (normative) and the item shapes no scene makes.
 * "scenes": calls built as bursts, through the tracker and the uplink (mqttnet_scenes.h): every message published,
 * its topic, and the activity events. test_mqttnet.c checks the C against all of it.
 */
#include <stdio.h>

#include "mqttnet_scenes.h"

static char buf[1 << 16];

int main(void)
{
    const int nh = sc_make_hands(), ns = sc_run_all();
    printf("{\n  \"about\": \"RadioFoundry Gateway MQTT network call messages (docs/spec/fw/mqtt-net.md), made by "
           "host-tests/fixtures/mqtt-net/gen_vectors.c. hex is the whole message; header and items are it decoded. "
           "An item's value is what follows its type and length, in hex. LC src/dst, csbk count, data_pdu fields and "
           "quality rssi (dBm) are decoded too. Scenes: calls heard on RF by node box-a on path 1, slot 1, root DMR; "
           "activity holds the box's events in order.\",\n");
    printf("  \"root\": \"DMR\",\n  \"node\": \"box-a\",\n  \"vectors\": [");
    for (int k = 0; k < nh; k++) {
        sc_msg_json(sc_hands[k].msg, sc_hands[k].len, buf, sizeof buf);
        printf("%s\n    {\"name\": \"%s\", \"what\": \"%s\", \"normative\": %s,\n     \"message\": %s}", k ? "," : "",
               sc_hands[k].name, sc_hands[k].what, !strncmp(sc_hands[k].name, "spec_", 5) ? "true" : "false", buf);
    }
    printf("\n  ],\n  \"scenes\": [");
    for (int s = 0; s < ns; s++) {
        const sc_out_t *o = &sc_outs[s];
        printf("%s\n    {\"name\": \"%s\", \"what\": \"%s\",\n     \"messages\": [", s ? "," : "", o->name, o->what);
        for (int m = 0; m < o->nmsg; m++) {
            char topic[96];
            rdof_mn_topic(topic, sizeof topic, "DMR", o->msg[m].kind, o->msg[m].dst, "box-a");
            sc_msg_json(o->msg[m].msg, o->msg[m].len, buf, sizeof buf);
            printf("%s\n      {\"topic\": \"%s\", \"message\": %s}", m ? "," : "", topic, buf);
        }
        printf("\n     ],\n     \"activity\": [");
        for (int e = 0; e < o->nev; e++)
            printf("%s\n      %s", e ? "," : "", o->ev[e]);
        printf("\n     ]}");
    }
    printf("\n  ]\n}\n");
    return 0;
}
