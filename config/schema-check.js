// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A small JSON Schema 2020-12 checker for config/rdof-config.schema.json: the keywords that schema uses, no more.
 * Runs in the browser (the settings editor) and in node (host-tests/test_schema.js, tools/config_check.py's
 * fallback). check(schema, value) -> [{path, message}], empty when it passes.
 *
 * Keywords: $ref (to #/$defs/...), type, enum, const, properties, additionalProperties, unevaluatedProperties (the
 * properties of the schema, its $ref and its allOf), required, propertyNames, min/maxProperties, items,
 * min/maxItems, uniqueItems, minimum, maximum, exclusiveMinimum, pattern, min/maxLength, allOf, anyOf, oneOf, not,
 * if/then/else. description and title are ignored.
 */
(function (root, factory) {
    if (typeof module === "object" && module.exports) module.exports = factory();
    else root.rdofSchemaCheck = factory();
})(typeof self !== "undefined" ? self : this, function () {
    "use strict";

    function typeOf(v) {
        if (v === null) return "null";
        if (Array.isArray(v)) return "array";
        if (typeof v === "number") return Number.isInteger(v) ? "integer" : "number";
        return typeof v;
    }

    function typeOk(want, v) {
        const t = typeOf(v);
        return want === t || (want === "number" && t === "integer");
    }

    function same(a, b) {
        return JSON.stringify(a) === JSON.stringify(b);
    }

    function resolve(rootSchema, ref) {
        if (!ref.startsWith("#/")) throw new Error("only local $ref: " + ref);
        let s = rootSchema;
        for (const part of ref.slice(2).split("/")) s = s[part];
        if (s === undefined) throw new Error("no " + ref);
        return s;
    }

    function join(path, key) {
        if (typeof key === "number") return path + "[" + key + "]";
        return path ? path + "." + key : key;
    }

    function check(rootSchema, value) {
        // The property names a schema evaluates without conditionals: its own, its $ref's and its allOf's.
        function evaluated(s, names) {
            if (s.properties) for (const k in s.properties) names.add(k);
            if (s.$ref) evaluated(resolve(rootSchema, s.$ref), names);
            if (s.allOf) s.allOf.forEach((x) => evaluated(x, names));
            return names;
        }

        function walk(s, v, path, errs) {
            if (s === true || s === undefined) return;
            if (s === false) {
                errs.push({path, message: "not allowed"});
                return;
            }
            if (s.$ref) walk(resolve(rootSchema, s.$ref), v, path, errs);
            if (s.type !== undefined) {
                const types = Array.isArray(s.type) ? s.type : [s.type];
                if (!types.some((t) => typeOk(t, v))) {
                    errs.push({path, message: "should be " + types.join(" or ")});
                    return;
                }
            }
            if (s.const !== undefined && !same(s.const, v))
                errs.push({path, message: "should be " + JSON.stringify(s.const)});
            if (s.enum && !s.enum.some((e) => same(e, v)))
                errs.push({path, message: "one of " + s.enum.map((e) => JSON.stringify(e)).join(", ")});
            const t = typeOf(v);
            if (t === "number" || t === "integer") {
                if (s.minimum !== undefined && v < s.minimum) errs.push({path, message: "at least " + s.minimum});
                if (s.maximum !== undefined && v > s.maximum) errs.push({path, message: "at most " + s.maximum});
                if (s.exclusiveMinimum !== undefined && v <= s.exclusiveMinimum)
                    errs.push({path, message: "more than " + s.exclusiveMinimum});
            }
            if (t === "string") {
                if (s.minLength !== undefined && v.length < s.minLength)
                    errs.push({path, message: "at least " + s.minLength + " characters"});
                if (s.maxLength !== undefined && v.length > s.maxLength)
                    errs.push({path, message: "at most " + s.maxLength + " characters"});
                if (s.pattern !== undefined && !new RegExp(s.pattern, "u").test(v))
                    errs.push({path, message: "doesn't match " + s.pattern});
            }
            if (t === "array") {
                if (s.minItems !== undefined && v.length < s.minItems)
                    errs.push({path, message: "at least " + s.minItems + " items"});
                if (s.maxItems !== undefined && v.length > s.maxItems)
                    errs.push({path, message: "at most " + s.maxItems + " items"});
                if (s.uniqueItems) {
                    const seen = new Set();
                    v.forEach((x, i) => {
                        const k = JSON.stringify(x);
                        if (seen.has(k)) errs.push({path: join(path, i), message: "twice"});
                        seen.add(k);
                    });
                }
                if (s.items !== undefined) v.forEach((x, i) => walk(s.items, x, join(path, i), errs));
            }
            if (t === "object") {
                const keys = Object.keys(v);
                if (s.required)
                    for (const k of s.required)
                        if (!(k in v)) errs.push({path: join(path, k), message: "missing"});
                if (s.minProperties !== undefined && keys.length < s.minProperties)
                    errs.push({path, message: "at least " + s.minProperties});
                if (s.maxProperties !== undefined && keys.length > s.maxProperties)
                    errs.push({path, message: "at most " + s.maxProperties});
                for (const k of keys) {
                    if (s.propertyNames) {
                        const e = [];
                        walk(s.propertyNames, k, join(path, k), e);
                        if (e.length) errs.push({path: join(path, k), message: "not a valid name"});
                    }
                    if (s.properties && k in s.properties) walk(s.properties[k], v[k], join(path, k), errs);
                    else if (s.additionalProperties === false) errs.push({path: join(path, k), message: "not a key here"});
                    else if (s.additionalProperties !== undefined) walk(s.additionalProperties, v[k], join(path, k), errs);
                }
                if (s.unevaluatedProperties === false) {
                    const names = evaluated(s, new Set());
                    for (const k of keys) if (!names.has(k)) errs.push({path: join(path, k), message: "not a key here"});
                }
            }
            if (s.allOf) s.allOf.forEach((x) => walk(x, v, path, errs));
            if (s.anyOf) {
                const tries = s.anyOf.map((x) => {
                    const e = [];
                    walk(x, v, path, e);
                    return e;
                });
                if (!tries.some((e) => !e.length)) errs.push(...best(tries, path));
            }
            if (s.oneOf) {
                const tries = s.oneOf.map((x) => {
                    const e = [];
                    walk(x, v, path, e);
                    return e;
                });
                const n = tries.filter((e) => !e.length).length;
                if (n === 0) errs.push(...best(tries, path));
                else if (n > 1) errs.push({path, message: "matches more than one form"});
            }
            if (s.not) {
                const e = [];
                walk(s.not, v, path, e);
                if (!e.length) errs.push({path, message: "not allowed in this form"});
            }
            if (s.if) {
                const e = [];
                walk(s.if, v, path, e);
                if (!e.length && s.then !== undefined) walk(s.then, v, path, errs);
                else if (e.length && s.else !== undefined) walk(s.else, v, path, errs);
            }
        }

        // Of several failed forms, the errors of the one that got furthest (fewest errors, deepest path).
        function best(tries, path) {
            const depth = (e) => Math.max(...e.map((x) => x.path.length));
            const sorted = tries.slice().sort((a, b) => depth(b) - depth(a) || a.length - b.length);
            const top = sorted[0];
            if (depth(top) <= path.length) return [{path, message: "none of its allowed forms"}];
            return top;
        }

        const errs = [];
        walk(rootSchema, value, "", errs);
        return errs;
    }

    return {check};
});
