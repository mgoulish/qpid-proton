/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "sasl-internal.h"

#include "dispatch_actions.h"
#include "engine/engine-internal.h"
#include "protocol.h"
#include "proton/ssl.h"
#include "util.h"
#include "transport/autodetect.h"

#include <assert.h>

static inline pn_transport_t *get_transport_internal(pn_sasl_t *sasl)
{
    // The external pn_sasl_t is really a pointer to the internal pni_transport_t
    return ((pn_transport_t *)sasl);
}

static ssize_t pn_sasl_input(pn_transport_t *transport, const char *bytes, size_t available);
static ssize_t pn_sasl_output(pn_transport_t *transport, char *bytes, size_t size);

static ssize_t pn_input_read_sasl_header(pn_transport_t* transport, unsigned int layer, const char* bytes, size_t available);
static ssize_t pn_input_read_sasl(pn_transport_t *transport, unsigned int layer, const char *bytes, size_t available);
static ssize_t pn_output_write_sasl_header(pn_transport_t* transport, unsigned int layer, char* bytes, size_t size);
static ssize_t pn_output_write_sasl(pn_transport_t *transport, unsigned int layer, char *bytes, size_t available);

const pn_io_layer_t sasl_header_layer = {
    pn_input_read_sasl_header,
    pn_output_write_sasl_header,
    NULL,
    NULL
};

const pn_io_layer_t sasl_write_header_layer = {
    pn_input_read_sasl,
    pn_output_write_sasl_header,
    NULL,
    NULL
};

const pn_io_layer_t sasl_read_header_layer = {
    pn_input_read_sasl_header,
    pn_output_write_sasl,
    NULL,
    NULL
};

const pn_io_layer_t sasl_layer = {
    pn_input_read_sasl,
    pn_output_write_sasl,
    NULL,
    NULL
};

#define SASL_HEADER ("AMQP\x03\x01\x00\x00")
#define SASL_HEADER_LEN 8

static ssize_t pn_input_read_sasl_header(pn_transport_t* transport, unsigned int layer, const char* bytes, size_t available)
{
  bool eos = pn_transport_capacity(transport)==PN_EOS;
  pni_protocol_type_t protocol = pni_sniff_header(bytes, available);
  switch (protocol) {
  case PNI_PROTOCOL_AMQP_SASL:
    if (transport->io_layers[layer] == &sasl_read_header_layer) {
        transport->io_layers[layer] = &sasl_layer;
    } else {
        transport->io_layers[layer] = &sasl_write_header_layer;
    }
    if (transport->trace & PN_TRACE_FRM)
        pn_transport_logf(transport, "  <- %s", "SASL");
    pni_sasl_set_external_security(transport, pn_ssl_get_ssf((pn_ssl_t*)transport), pn_ssl_get_remote_subject((pn_ssl_t*)transport));
    return SASL_HEADER_LEN;
  case PNI_PROTOCOL_INSUFFICIENT:
    if (!eos) return 0;
    /* Fallthru */
  default:
    break;
  }
  transport->close_sent = true;
  char quoted[1024];
  pn_quote_data(quoted, 1024, bytes, available);
  pn_do_error(transport, "amqp:connection:framing-error",
              "%s header mismatch: %s ['%s']%s", "SASL", pni_protocol_name(protocol), quoted,
              !eos ? "" : " (connection aborted)");
  pn_set_error_layer(transport);
  return PN_EOS;
}

static ssize_t pn_input_read_sasl(pn_transport_t* transport, unsigned int layer, const char* bytes, size_t available)
{
  bool eos = pn_transport_capacity(transport)==PN_EOS;
  if (eos) {
    transport->close_sent = true;
    pn_do_error(transport, "amqp:connection:framing-error", "connection aborted");
    pn_set_error_layer(transport);
    return PN_EOS;
  }

  if (!transport->sasl->input_bypass) {
    ssize_t n = pn_sasl_input(transport, bytes, available);
    if (n != PN_EOS) return n;

    transport->sasl->input_bypass = true;
    if (transport->sasl->output_bypass)
        transport->io_layers[layer] = &pni_passthru_layer;
  }
  return pni_passthru_layer.process_input(transport, layer, bytes, available );
}

static ssize_t pn_output_write_sasl_header(pn_transport_t *transport, unsigned int layer, char *bytes, size_t size)
{
  if (transport->trace & PN_TRACE_FRM)
    pn_transport_logf(transport, "  -> %s", "SASL");
  assert(size >= SASL_HEADER_LEN);
  memmove(bytes, SASL_HEADER, SASL_HEADER_LEN);
  if (transport->io_layers[layer]==&sasl_write_header_layer) {
      transport->io_layers[layer] = &sasl_layer;
  } else {
      transport->io_layers[layer] = &sasl_read_header_layer;
  }
  return SASL_HEADER_LEN;
}

static ssize_t pn_output_write_sasl(pn_transport_t* transport, unsigned int layer, char* bytes, size_t available)
{
  if (!transport->sasl->output_bypass) {
    // this accounts for when pn_do_error is invoked, e.g. by idle timeout
    ssize_t n;
    if (transport->close_sent) {
        n = PN_EOS;
    } else {
        n = pn_sasl_output(transport, bytes, available);
    }
    if (n != PN_EOS) return n;

    transport->sasl->output_bypass = true;
    if (transport->sasl->input_bypass)
        transport->io_layers[layer] = &pni_passthru_layer;
  }
  return pni_passthru_layer.process_output(transport, layer, bytes, available );
}

static bool pni_sasl_is_server_state(enum pni_sasl_state state)
{
  return state==SASL_NONE
      || state==SASL_POSTED_MECHANISMS
      || state==SASL_POSTED_CHALLENGE
      || state==SASL_POSTED_OUTCOME;
}

static bool pni_sasl_is_client_state(enum pni_sasl_state state)
{
  return state==SASL_NONE
      || state==SASL_POSTED_INIT
      || state==SASL_POSTED_RESPONSE
      || state==SASL_PRETEND_OUTCOME
      || state==SASL_RECVED_OUTCOME;
}

static bool pni_sasl_is_final_input_state(pni_sasl_t *sasl)
{
  enum pni_sasl_state last_state = sasl->last_state;
  enum pni_sasl_state desired_state = sasl->desired_state;
  return last_state==SASL_RECVED_OUTCOME
      || desired_state==SASL_POSTED_OUTCOME;
}

static bool pni_sasl_is_final_output_state(pni_sasl_t *sasl)
{
  enum pni_sasl_state last_state = sasl->last_state;
  return last_state==SASL_PRETEND_OUTCOME
      || last_state==SASL_RECVED_OUTCOME
      || last_state==SASL_POSTED_OUTCOME;
}

static inline pni_sasl_t *get_sasl_internal(pn_sasl_t *sasl)
{
    // The external pn_sasl_t is really a pointer to the internal pni_transport_t
    return sasl ? ((pn_transport_t *)sasl)->sasl : NULL;
}

static void pni_emit(pn_transport_t *transport)
{
  if (transport->connection && transport->connection->collector) {
    pn_collector_t *collector = transport->connection->collector;
    pn_collector_put(collector, PN_OBJECT, transport, PN_TRANSPORT);
  }
}

// Look for symbol in the mech include list - not particlarly efficient,
// but probably not used enough to matter.
//
// Note that if there is no inclusion list then every mech is implicitly included.
bool pni_included_mech(const char *included_mech_list, pn_bytes_t s)
{
  if (!included_mech_list) return true;

  const char * end_list = included_mech_list+strlen(included_mech_list);
  size_t len = s.size;
  const char *c = included_mech_list;
  while (c!=NULL) {
    // If there are not enough chars left in the list no matches
    if ((ptrdiff_t)len > end_list-c) return false;

    // Is word equal with a space or end of string afterwards?
    if (pn_strncasecmp(c, s.start, len)==0 && (c[len]==' ' || c[len]==0) ) return true;

    c = strchr(c, ' ');
    c = c ? c+1 : NULL;
  }
  return false;
}

// This takes a space separated list and zero terminates it in place
// whilst adding pointers to the existing strings in a string array.
// This means that you can't free the original storage until you have
// finished with the resulting list.
void pni_split_mechs(char *mechlist, const char* included_mechs, char *mechs[], int *count)
{
  char *start = mechlist;
  char *end = start;

  while (*end) {
    if (*end == ' ') {
      if (start != end) {
        *end = '\0';
        if (pni_included_mech(included_mechs, pn_bytes(end-start, start))) {
          mechs[(*count)++] = start;
        }
      }
      end++;
      start = end;
    } else {
      end++;
    }
  }

  if (start != end) {
    if (pni_included_mech(included_mechs, pn_bytes(end-start, start))) {
      mechs[(*count)++] = start;
    }
  }
}

void pni_sasl_set_desired_state(pn_transport_t *transport, enum pni_sasl_state desired_state)
{
  pni_sasl_t *sasl = transport->sasl;
  if (sasl->last_state > desired_state) {
    pn_transport_logf(transport, "Trying to send SASL frame (%d), but illegal: already in later state (%d)", desired_state, sasl->last_state);
  } else if (sasl->client && !pni_sasl_is_client_state(desired_state)) {
    pn_transport_logf(transport, "Trying to send server SASL frame (%d) on a client", desired_state);
  } else if (!sasl->client && !pni_sasl_is_server_state(desired_state)) {
    pn_transport_logf(transport, "Trying to send client SASL frame (%d) on a server", desired_state);
  } else {
    // If we need to repeat CHALLENGE or RESPONSE frames adjust current state to seem
    // like they haven't been sent yet
    if (sasl->last_state==desired_state && desired_state==SASL_POSTED_RESPONSE) {
      sasl->last_state = SASL_POSTED_INIT;
    }
    if (sasl->last_state==desired_state && desired_state==SASL_POSTED_CHALLENGE) {
      sasl->last_state = SASL_POSTED_MECHANISMS;
    }
    sasl->desired_state = desired_state;
    pni_emit(transport);
  }
}

// Post SASL frame
static void pni_post_sasl_frame(pn_transport_t *transport)
{
  pni_sasl_t *sasl = transport->sasl;
  pn_bytes_t out = sasl->bytes_out;
  enum pni_sasl_state desired_state = sasl->desired_state;
  while (sasl->desired_state > sasl->last_state) {
    switch (desired_state) {
    case SASL_POSTED_INIT:
      pn_post_frame(transport, SASL_FRAME_TYPE, 0, "DL[sz]", SASL_INIT, sasl->selected_mechanism,
                    out.size, out.start);
      pni_emit(transport);
      break;
    case SASL_PRETEND_OUTCOME:
      if (sasl->last_state < SASL_POSTED_INIT) {
        desired_state = SASL_POSTED_INIT;
        continue;
      }
      break;
    case SASL_POSTED_MECHANISMS: {
      // TODO: Hardcoded limit of 16 mechanisms
      char *mechs[16];
      char *mechlist = NULL;

      int count = 0;
      if (pni_sasl_impl_list_mechs(transport, &mechlist) > 0) {
        pni_split_mechs(mechlist, sasl->included_mechanisms, mechs, &count);
      }

      pn_post_frame(transport, SASL_FRAME_TYPE, 0, "DL[@T[*s]]", SASL_MECHANISMS, PN_SYMBOL, count, mechs);
      free(mechlist);
      pni_emit(transport);
      break;
    }
    case SASL_POSTED_RESPONSE:
      pn_post_frame(transport, SASL_FRAME_TYPE, 0, "DL[z]", SASL_RESPONSE, out.size, out.start);
      pni_emit(transport);
      break;
    case SASL_POSTED_CHALLENGE:
      if (sasl->last_state < SASL_POSTED_MECHANISMS) {
        desired_state = SASL_POSTED_MECHANISMS;
        continue;
      }
      pn_post_frame(transport, SASL_FRAME_TYPE, 0, "DL[z]", SASL_CHALLENGE, out.size, out.start);
      pni_emit(transport);
      break;
    case SASL_POSTED_OUTCOME:
      if (sasl->last_state < SASL_POSTED_MECHANISMS) {
        desired_state = SASL_POSTED_MECHANISMS;
        continue;
      }
      pn_post_frame(transport, SASL_FRAME_TYPE, 0, "DL[B]", SASL_OUTCOME, sasl->outcome);
      pni_emit(transport);
      break;
    case SASL_RECVED_OUTCOME:
      if (sasl->last_state < SASL_POSTED_INIT && sasl->outcome==PN_SASL_OK) {
        desired_state = SASL_POSTED_INIT;
        continue;
      }
      break;
    case SASL_NONE:
      return;
    }
    sasl->last_state = desired_state;
    desired_state = sasl->desired_state;
  }
}

pn_sasl_t *pn_sasl(pn_transport_t *transport)
{
  if (!transport->sasl) {
    pni_sasl_t *sasl = (pni_sasl_t *) malloc(sizeof(pni_sasl_t));

    const char *sasl_config_path = getenv("PN_SASL_CONFIG_PATH");

    sasl->impl_context = NULL;
    sasl->client = !transport->server;
    sasl->selected_mechanism = NULL;
    sasl->included_mechanisms = NULL;
    sasl->username = NULL;
    sasl->password = NULL;
    sasl->config_name = sasl->client ? "proton-client" : "proton-server";
    sasl->config_dir =  sasl_config_path ? pn_strdup(sasl_config_path) : NULL;
    sasl->remote_fqdn = NULL;
    sasl->external_auth = NULL;
    sasl->external_ssf = 0;
    sasl->outcome = PN_SASL_NONE;
    sasl->impl_context = NULL;
    sasl->bytes_out.size = 0;
    sasl->bytes_out.start = NULL;
    sasl->desired_state = SASL_NONE;
    sasl->last_state = SASL_NONE;
    sasl->input_bypass = false;
    sasl->output_bypass = false;

    transport->sasl = sasl;
  }

  // The actual external pn_sasl_t pointer is a pointer to its enclosing pn_transport_t
  return (pn_sasl_t *)transport;
}

// This is a hack to tell us that
// no actual negotiation is going to happen and we can go
// straight to the AMQP layer; it can only work on the client side
// As the server doesn't know if SASL is even active until it sees
// the SASL header from the client first.
static void pni_sasl_force_anonymous(pn_transport_t *transport)
{
  pni_sasl_t *sasl = transport->sasl;
  if (sasl->client) {
    // Pretend we got sasl mechanisms frame with just ANONYMOUS
    if (pni_init_client(transport) &&
        pni_process_mechanisms(transport, "ANONYMOUS")) {
      pni_sasl_set_desired_state(transport, SASL_PRETEND_OUTCOME);
    } else {
      sasl->outcome = PN_SASL_PERM;
      pni_sasl_set_desired_state(transport, SASL_RECVED_OUTCOME);
    }
  }
}

void pni_sasl_set_remote_hostname(pn_transport_t * transport, const char * fqdn)
{
  pni_sasl_t *sasl = transport->sasl;
  sasl->remote_fqdn = fqdn;
}

void pni_sasl_set_user_password(pn_transport_t *transport, const char *user, const char *password)
{
  pni_sasl_t *sasl = transport->sasl;
  sasl->username = user;
  free(sasl->password);
  sasl->password = password ? pn_strdup(password) : NULL;
}

void pni_sasl_set_external_security(pn_transport_t *transport, int ssf, const char *authid)
{
  pni_sasl_t *sasl = transport->sasl;
  sasl->external_ssf = ssf;
  free(sasl->external_auth);
  sasl->external_auth = authid ? pn_strdup(authid) : NULL;
}

const char *pn_sasl_get_user(pn_sasl_t *sasl0)
{
    pni_sasl_t *sasl = get_sasl_internal(sasl0);
    return sasl->username;
}

const char *pn_sasl_get_mech(pn_sasl_t *sasl0)
{
    pni_sasl_t *sasl = get_sasl_internal(sasl0);
    return sasl->selected_mechanism;
}

void pn_sasl_allowed_mechs(pn_sasl_t *sasl0, const char *mechs)
{
    pni_sasl_t *sasl = get_sasl_internal(sasl0);
    free(sasl->included_mechanisms);
    sasl->included_mechanisms = mechs ? pn_strdup(mechs) : NULL;
    if (strcmp(mechs, "ANONYMOUS")==0 ) {
      pn_transport_t *transport = get_transport_internal(sasl0);
      pni_sasl_force_anonymous(transport);
    }
}

void pn_sasl_config_name(pn_sasl_t *sasl0, const char *name)
{
    pni_sasl_t *sasl = get_sasl_internal(sasl0);
    sasl->config_name = name;
}

void pn_sasl_config_path(pn_sasl_t *sasl0, const char *dir)
{
    pni_sasl_t *sasl = get_sasl_internal(sasl0);
    free(sasl->config_dir);
    sasl->config_dir = pn_strdup(dir);
}

void pn_sasl_done(pn_sasl_t *sasl0, pn_sasl_outcome_t outcome)
{
  pni_sasl_t *sasl = get_sasl_internal(sasl0);
  if (sasl) {
    sasl->outcome = outcome;
  }
}

pn_sasl_outcome_t pn_sasl_outcome(pn_sasl_t *sasl0)
{
  pni_sasl_t *sasl = get_sasl_internal(sasl0);
  return sasl ? sasl->outcome : PN_SASL_NONE;
}

void pn_sasl_free(pn_transport_t *transport)
{
  if (transport) {
    pni_sasl_t *sasl = transport->sasl;
    if (sasl) {
      free(sasl->selected_mechanism);
      free(sasl->included_mechanisms);
      free(sasl->password);
      free(sasl->config_dir);
      free(sasl->external_auth);

      // CYRUS_SASL
      if (sasl->impl_context) {
          pni_sasl_impl_free(transport);
      }

      free(sasl);
    }
  }
}

static void pni_sasl_server_init(pn_transport_t *transport)
{
  if (!pni_init_server(transport)) return;

  // Setup to send SASL mechanisms frame
  pni_sasl_set_desired_state(transport, SASL_POSTED_MECHANISMS);
}

static void pn_sasl_process(pn_transport_t *transport)
{
  pni_sasl_t *sasl = transport->sasl;
  if (!sasl->client) {
    if (sasl->desired_state<SASL_POSTED_MECHANISMS) {
      pni_sasl_server_init(transport);
    }
  }
}

ssize_t pn_sasl_input(pn_transport_t *transport, const char *bytes, size_t available)
{
  pn_sasl_process(transport);

  pni_sasl_t *sasl = transport->sasl;
  bool dummy = false;
  ssize_t n = pn_dispatcher_input(transport, bytes, available, false, &dummy);

  if (n==0 && pni_sasl_is_final_input_state(sasl)) {
    return PN_EOS;
  }
  return n;
}

ssize_t pn_sasl_output(pn_transport_t *transport, char *bytes, size_t size)
{
  pn_sasl_process(transport);

  pni_post_sasl_frame(transport);

  pni_sasl_t *sasl = transport->sasl;
  if (transport->available == 0 && pni_sasl_is_final_output_state(sasl)) {
    if (sasl->outcome != PN_SASL_OK && pni_sasl_is_final_input_state(sasl)) {
      pn_transport_close_tail(transport);
    }
    return PN_EOS;
  } else {
    return pn_dispatcher_output(transport, bytes, size);
  }
}

// Received Server side
int pn_do_init(pn_transport_t *transport, uint8_t frame_type, uint16_t channel, pn_data_t *args, const pn_bytes_t *payload)
{
  pni_sasl_t *sasl = transport->sasl;
  pn_bytes_t mech;
  pn_bytes_t recv;
  int err = pn_data_scan(args, "D.[sz]", &mech, &recv);
  if (err) return err;
  sasl->selected_mechanism = pn_strndup(mech.start, mech.size);

  pni_process_init(transport, sasl->selected_mechanism, &recv);

  return 0;
}

// Received client side
int pn_do_mechanisms(pn_transport_t *transport, uint8_t frame_type, uint16_t channel, pn_data_t *args, const pn_bytes_t *payload)
{
  pni_sasl_t *sasl = transport->sasl;

  // If we already pretended we got the ANONYMOUS mech then ignore
  if (sasl->last_state==SASL_PRETEND_OUTCOME) return 0;

  // This scanning relies on pn_data_scan leaving the pn_data_t cursors
  // where they are after finishing the scan
  int err = pn_data_scan(args, "D.[@[");
  if (err) return err;

  pn_string_t *mechs = pn_string("");

  // Now keep checking for end of array and pull a symbol
  while(pn_data_next(args)) {
    pn_bytes_t s = pn_data_get_symbol(args);
    if (pni_included_mech(transport->sasl->included_mechanisms, s)) {
      pn_string_addf(mechs, "%*s ", (int)s.size, s.start);
    }
  }

  if (pn_string_size(mechs)) {
      pn_string_buffer(mechs)[pn_string_size(mechs)-1] = 0;
  }

  if (pni_init_client(transport) &&
      pni_process_mechanisms(transport, pn_string_get(mechs))) {
    pni_sasl_set_desired_state(transport, SASL_POSTED_INIT);
  } else {
    sasl->outcome = PN_SASL_PERM;
    pni_sasl_set_desired_state(transport, SASL_RECVED_OUTCOME);
  }

  pn_free(mechs);
  return 0;
}

// Received client side
int pn_do_challenge(pn_transport_t *transport, uint8_t frame_type, uint16_t channel, pn_data_t *args, const pn_bytes_t *payload)
{
  pn_bytes_t recv;
  int err = pn_data_scan(args, "D.[z]", &recv);
  if (err) return err;

  pni_process_challenge(transport, &recv);

  return 0;
}

// Received server side
int pn_do_response(pn_transport_t *transport, uint8_t frame_type, uint16_t channel, pn_data_t *args, const pn_bytes_t *payload)
{
  pn_bytes_t recv;
  int err = pn_data_scan(args, "D.[z]", &recv);
  if (err) return err;

  pni_process_response(transport, &recv);

  return 0;
}

// Received client side
int pn_do_outcome(pn_transport_t *transport, uint8_t frame_type, uint16_t channel, pn_data_t *args, const pn_bytes_t *payload)
{
  uint8_t outcome;
  int err = pn_data_scan(args, "D.[B]", &outcome);
  if (err) return err;

  pni_sasl_t *sasl = transport->sasl;
  sasl->outcome = (pn_sasl_outcome_t) outcome;
  transport->authenticated = sasl->outcome==PN_SASL_OK;
  pni_sasl_set_desired_state(transport, SASL_RECVED_OUTCOME);

  return 0;
}


