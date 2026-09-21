/*
 * The CA roots compiled into the binary.
 *
 * What the TLS client checks every public server against when nobody named a bundle, so a root
 * that does not parse is a CA this client cannot reach - and would say so only as "certificate
 * not trusted" against whichever site happened to chain to it. Whether the table matches the
 * pak's PEM is `gen-ca-roots.py --check`, a ctest of its own; this is whether Mbed TLS, as this
 * build configures it, can read what is in it.
 */

#include "framework/mesh_test.h"

#include "mesh/core/ca_roots.h"

#ifdef INKWELL_HAVE_TLS

#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <stdbool.h>
#include <stdio.h>

/*
 * Every one, one at a time, so a failure names the root rather than reporting a count. The
 * build's configuration takes things out of Mbed TLS - curves, key types - and a Mozilla root
 * using one of them would only show up here.
 */
MESH_TEST_CASE(ca_roots_all_parse, unit) {
    if (psa_crypto_init() != PSA_SUCCESS) {
        record_failure(test_name, "PSA did not initialise");
        return;
    }
    if (mesh_ca_root_count < 50U) {
        record_failure(test_name, "the table should hold Mozilla's roots, not a handful");
        return;
    }
    for (size_t i = 0U; i < mesh_ca_root_count; ++i) {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        const int rc =
            mbedtls_x509_crt_parse_der_nocopy(&crt, mesh_ca_roots[i].der, mesh_ca_roots[i].len);
        /* A root is a CA, or the chain would stop at it without trusting anything beneath. */
        const bool is_ca = rc == 0 && mbedtls_x509_crt_get_ca_istrue(&crt) == 1;
        mbedtls_x509_crt_free(&crt);
        if (rc != 0 || !is_ca) {
            char why[160];
            snprintf(why, sizeof why, "\"%s\" %s (%d)", mesh_ca_roots[i].name,
                     rc != 0 ? "did not parse" : "is not a CA", rc);
            record_failure(test_name, why);
            return;
        }
    }
    record_success(test_name);
}

#endif /* INKWELL_HAVE_TLS */
