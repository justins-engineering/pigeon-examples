/*
 * Checks the carrier half of Non-IP Data Delivery from a bench board: attach
 * over NB-IoT, bring up a Non-IP PDN, print every downlink, and send test
 * frames from the shell.
 */
#include <errno.h>
#include <modem/at_monitor.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(nidd_probe, CONFIG_NIDD_PROBE_LOG_LEVEL);

/* Above Verizon's 1358-byte ceiling, so a send can find where the modem or the
 * network draws its own line. */
#define FRAME_MAX 2048
#define CLAIM_KEY_LEN 16
#define TAG_LEN 8
#define FRAME_TELEMETRY 0x01
#define FRAME_SHADOW_REPORT 0x02
#define FRAME_HELLO 0x04
/* Platform frame types have the top bit set, and each ends in a tag. */
#define FRAME_FROM_PLATFORM 0x80
#define FRAME_SHADOW 0x81
#define FRAME_STATUS 0x82
/* Type, then target_version and current_version as little-endian i32. */
#define SHADOW_HEADER_LEN 9
/* Type, code, then arg as a little-endian u32, then the tag. */
#define STATUS_LEN 14
#define STATUS_STORED 0x00
#define STATUS_PAUSED 0x01
#define STATUS_UNCLAIMED 0x02

/* Past this, waiting will not produce an NB-IoT attach. The modem is powered
 * off rather than reset: repeated ungraceful resets bar attach for 30 minutes. */
#define REGISTER_TIMEOUT_MIN 10
#define PDN_TIMEOUT_SEC 60

static K_SEM_DEFINE(registered, 0, 1);
static K_SEM_DEFINE(pdn_up, 0, 1);

static uint8_t nidd_cid;
static int nidd_fd = -1;
static uint8_t claim_key[CLAIM_KEY_LEN];
static bool claim_key_loaded;
static psa_key_id_t tag_key = PSA_KEY_ID_NULL;
static uint8_t rx_buf[FRAME_MAX];
static uint8_t tx_buf[FRAME_MAX];

/* Every unsolicited line the modem prints, verbatim: registration, PDN and
 * rate-control notices are read from these. */
AT_MONITOR(nidd_probe_notif, ANY, on_modem_notif);

static void on_modem_notif(const char* notif) {
  LOG_INF("modem: %.*s", (int)strcspn(notif, "\r\n"), notif);
}

/* Logs a command's response one line at a time. */
static void at_log(const char* cmd) {
  static char resp[512];
  int err = nrf_modem_at_cmd(resp, sizeof(resp), "%s", cmd);

  if (err < 0) {
    LOG_ERR("%s: %d", cmd, err);
    return;
  }
  if (err > 0) {
    LOG_ERR("%s: error type %d, code %d", cmd, nrf_modem_at_err_type(err), nrf_modem_at_err(err));
    return;
  }

  for (const char* line = resp; *line != '\0';) {
    size_t len = strcspn(line, "\r\n");

    if (len > 0 && !(len == 2 && memcmp(line, "OK", 2) == 0)) {
      LOG_INF("%s: %.*s", cmd, (int)len, line);
    }
    line += len;
    line += strspn(line, "\r\n");
  }
}

static void at_log_cid(const char* prefix) {
  char cmd[24];

  (void)snprintf(cmd, sizeof(cmd), "%s%u", prefix, nidd_cid);
  at_log(cmd);
}

/* The SIM's home network, from the first six digits of its IMSI (MCC and a three-digit MNC in
 * North America), tells whether the SIM is the carrier's at all. The rest of the IMSI names the
 * subscriber and stays off the console. Needs the SIM powered, so not before the modem is on. */
static void home_network_log(void) {
  char imsi[20];
  int err = nrf_modem_at_scanf("AT+CIMI", "%19s", imsi);

  if (err != 1) {
    LOG_ERR("AT+CIMI: %d", err);
    return;
  }
  LOG_INF("SIM home network (IMSI prefix): %.6s", imsi);
}

static void lte_handler(const struct lte_lc_evt* const evt) {
  switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
      if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
          evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
        k_sem_give(&registered);
      }
      break;
    case LTE_LC_EVT_LTE_MODE_UPDATE:
      LOG_INF(
          "LTE mode: %s", evt->lte_mode == LTE_LC_LTE_MODE_NBIOT  ? "NB-IoT"
                          : evt->lte_mode == LTE_LC_LTE_MODE_LTEM ? "LTE-M"
                                                                  : "none"
      );
      break;
    case LTE_LC_EVT_PSM_UPDATE:
      LOG_INF(
          "PSM from the network: TAU %d s, active time %d s", evt->psm_cfg.tau,
          evt->psm_cfg.active_time
      );
      break;
    case LTE_LC_EVT_PDN:
      if (evt->pdn.cid != nidd_cid) {
        break;
      }
      if (evt->pdn.type == LTE_LC_EVT_PDN_ACTIVATED) {
        k_sem_give(&pdn_up);
      } else if (evt->pdn.type == LTE_LC_EVT_PDN_ESM_ERROR) {
        LOG_WRN(
            "ESM error %d on CID %u: %s", evt->pdn.esm_err, nidd_cid,
            lte_lc_pdn_esm_strerror(evt->pdn.esm_err)
        );
      }
      break;
    default:
      break;
  }
}

static psa_key_id_t hmac_key_import(const uint8_t* key) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = PSA_KEY_ID_NULL;
  psa_status_t status;

  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attr, 8 * CLAIM_KEY_LEN);

  status = psa_import_key(&attr, key, CLAIM_KEY_LEN, &id);
  if (status != PSA_SUCCESS) {
    LOG_ERR("psa_import_key: %d", status);
  }

  return id;
}

/* The tag is the first TAG_LEN bytes of HMAC-SHA256 over everything before it,
 * compared without an early exit. */
static bool tag_verifies(psa_key_id_t key, const uint8_t* frame, size_t len) {
  uint8_t mac[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
  size_t mac_len;
  uint8_t diff = 0;
  psa_status_t status = psa_mac_compute(
      key, PSA_ALG_HMAC(PSA_ALG_SHA_256), frame, len - TAG_LEN, mac, sizeof(mac), &mac_len
  );

  if (status != PSA_SUCCESS) {
    LOG_ERR("psa_mac_compute: %d", status);
    return false;
  }
  for (size_t i = 0; i < TAG_LEN; i++) {
    diff |= mac[i] ^ frame[len - TAG_LEN + i];
  }

  return diff == 0;
}

/* The API reference's STATUS STORED 7 example, tagged with a key of 16 zero
 * bytes: checking it first shows that a "tag bad" later is the frame's fault. */
static void tag_self_test(void) {
  static const uint8_t zero_key[CLAIM_KEY_LEN];
  static const uint8_t example[STATUS_LEN] = {0x82, 0x00, 0x07, 0x00, 0x00, 0x00, 0xca,
                                              0x2f, 0xa8, 0x6d, 0x9c, 0xdc, 0xf1, 0x9b};
  psa_key_id_t id = hmac_key_import(zero_key);

  if (id == PSA_KEY_ID_NULL) {
    return;
  }
  LOG_INF(
      "Tag check on the API reference example: %s",
      tag_verifies(id, example, sizeof(example)) ? "ok" : "bad"
  );
  (void)psa_destroy_key(id);
}

/* Without a key the probe still runs; only HELLO and the tag check need it. */
static void claim_key_load(void) {
  const char* hex = CONFIG_NIDD_PROBE_CLAIM_KEY;
  psa_status_t status;

  if (strlen(hex) == 2 * CLAIM_KEY_LEN &&
      hex2bin(hex, 2 * CLAIM_KEY_LEN, claim_key, sizeof(claim_key)) == CLAIM_KEY_LEN) {
    claim_key_loaded = true;
  } else {
    LOG_WRN("CONFIG_NIDD_PROBE_CLAIM_KEY is not 32 hex digits: no HELLO, no tag checks");
  }

  status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    LOG_ERR("psa_crypto_init: %d", status);
    return;
  }
  tag_self_test();
  if (claim_key_loaded) {
    tag_key = hmac_key_import(claim_key);
  }
}

static const char* status_name(uint8_t code) {
  switch (code) {
    case STATUS_STORED:
      return "STORED";
    case STATUS_PAUSED:
      return "PAUSED";
    case STATUS_UNCLAIMED:
      return "UNCLAIMED";
    default:
      return "unknown";
  }
}

static void downlink_log(const uint8_t* frame, size_t len) {
  LOG_INF("Downlink, %zu bytes", len);
  LOG_HEXDUMP_INF(frame, len, "downlink");

  if (len <= TAG_LEN || (frame[0] & FRAME_FROM_PLATFORM) == 0) {
    LOG_INF("Downlink tag none: not a platform frame");
    return;
  }
  if (frame[0] == FRAME_SHADOW && len >= SHADOW_HEADER_LEN + TAG_LEN) {
    LOG_INF(
        "SHADOW target_version %d, current_version %d, target_config %zu bytes",
        (int)(int32_t)sys_get_le32(&frame[1]), (int)(int32_t)sys_get_le32(&frame[5]),
        len - SHADOW_HEADER_LEN - TAG_LEN
    );
  } else if (frame[0] == FRAME_STATUS && len == STATUS_LEN) {
    LOG_INF("STATUS %s, arg %u", status_name(frame[1]), (unsigned int)sys_get_le32(&frame[2]));
  } else {
    LOG_INF("Platform frame of unknown type 0x%02x or length", frame[0]);
  }

  if (tag_key == PSA_KEY_ID_NULL) {
    LOG_WRN("Downlink tag unchecked: no claim key");
  } else {
    LOG_INF("Downlink tag %s", tag_verifies(tag_key, frame, len) ? "ok" : "bad");
  }
}

static int frame_send(const struct shell* sh, const uint8_t* frame, size_t len) {
  ssize_t ret;

  if (nidd_fd < 0) {
    shell_error(sh, "The Non-IP socket is not open");
    return -ENOTCONN;
  }

  ret = zsock_send(nidd_fd, frame, len, 0);
  if (ret < 0) {
    LOG_ERR("send(%zu bytes) returned %zd, errno %d", len, ret, errno);
    return -errno;
  }
  LOG_INF("send(%zu bytes) returned %zd", len, ret);

  return 0;
}

/* Byte 0 is 0x00, a type reserved in both directions, so filler is never read
 * as a frame; the counting pattern shows truncation in what arrives. */
static int cmd_raw(const struct shell* sh, size_t argc, char** argv) {
  char* end;
  unsigned long len = strtoul(argv[1], &end, 10);

  ARG_UNUSED(argc);

  if (*end != '\0' || len == 0 || len > sizeof(tx_buf)) {
    shell_error(sh, "Size must be 1 to %u bytes", (unsigned int)sizeof(tx_buf));
    return -EINVAL;
  }
  for (size_t i = 0; i < len; i++) {
    tx_buf[i] = (uint8_t)i;
  }

  return frame_send(sh, tx_buf, len);
}

static int cmd_hello(const struct shell* sh, size_t argc, char** argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  if (!claim_key_loaded) {
    shell_error(sh, "No claim key: set CONFIG_NIDD_PROBE_CLAIM_KEY in prj.local.conf");
    return -ENOENT;
  }
  tx_buf[0] = FRAME_HELLO;
  memcpy(&tx_buf[1], claim_key, CLAIM_KEY_LEN);

  return frame_send(sh, tx_buf, 1 + CLAIM_KEY_LEN);
}

/* Each frame carries the next sequence number, so a burst shows which frames
 * were lost on the way. */
static int cmd_telemetry(const struct shell* sh, size_t argc, char** argv) {
  static unsigned int seq;
  int len;

  ARG_UNUSED(argc);
  ARG_UNUSED(argv);

  seq++;
  tx_buf[0] = FRAME_TELEMETRY;
  len = snprintf(
      (char*)&tx_buf[1], sizeof(tx_buf) - 1, "{\"probe_seq\":\"%u\",\"uptime_s\":\"%u\"}", seq,
      (unsigned int)(k_uptime_get() / MSEC_PER_SEC)
  );
  LOG_INF("TELEMETRY probe_seq %u", seq);

  return frame_send(sh, tx_buf, 1 + len);
}

/* The platform judges convergence by version alone, so the report carries an
 * empty config rather than echo one that may not fit a frame. */
static int cmd_report(const struct shell* sh, size_t argc, char** argv) {
  char* end;
  long version = strtol(argv[1], &end, 10);
  int len;

  ARG_UNUSED(argc);

  if (*end != '\0' || version < 0 || version > INT32_MAX) {
    shell_error(sh, "Version must be 0 to 2147483647");
    return -EINVAL;
  }
  tx_buf[0] = FRAME_SHADOW_REPORT;
  len = snprintf(
      (char*)&tx_buf[1], sizeof(tx_buf) - 1, "{\"current_config\":{},\"current_version\":%ld}",
      version
  );

  return frame_send(sh, tx_buf, 1 + len);
}

static int cmd_psm(const struct shell* sh, size_t argc, char** argv) {
  bool enable = strcmp(argv[1], "on") == 0;
  int err;

  ARG_UNUSED(argc);

  if (!enable && strcmp(argv[1], "off") != 0) {
    shell_error(sh, "Say on or off");
    return -EINVAL;
  }
  err = lte_lc_psm_req(enable);
  LOG_INF("PSM request %s: %d", argv[1], err);

  return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    nidd_cmds, SHELL_CMD_ARG(raw, NULL, "<bytes> Send that many filler bytes", cmd_raw, 2, 0),
    SHELL_CMD(hello, NULL, "Send HELLO carrying the claim key", cmd_hello),
    SHELL_CMD(telemetry, NULL, "Send TELEMETRY with a sequence number", cmd_telemetry),
    SHELL_CMD_ARG(report, NULL, "<version> Send SHADOW_REPORT for that version", cmd_report, 2, 0),
    SHELL_CMD_ARG(psm, NULL, "<on|off> Request PSM or stop requesting it", cmd_psm, 2, 0),
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(nidd, &nidd_cmds, "NIDD probe frames", NULL);

static int socket_open(void) {
  int pdn_id = lte_lc_pdn_id_get(nidd_cid);
  int fd;

  if (pdn_id < 0) {
    LOG_ERR("lte_lc_pdn_id_get: %d", pdn_id);
    return pdn_id;
  }
  LOG_INF("PDN ID %d on CID %u", pdn_id, nidd_cid);

  fd = zsock_socket(NET_AF_PACKET, NET_SOCK_RAW, 0);
  if (fd < 0) {
    LOG_ERR("zsock_socket: errno %d", errno);
    return -errno;
  }

  /* An unbound raw socket rides the default PDN. */
  if (IS_ENABLED(CONFIG_NIDD_PROBE_DEDICATED_CID) &&
      zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, SO_BINDTOPDN, &pdn_id, sizeof(pdn_id)) != 0) {
    LOG_ERR("SO_BINDTOPDN: errno %d", errno);
    (void)zsock_close(fd);
    return -errno;
  }

  return fd;
}

int main(void) {
  int err = nrf_modem_lib_init();

  if (err) {
    LOG_ERR("nrf_modem_lib_init: %d", err);
    return 0;
  }

  at_log("AT+CGMR");
  at_log("AT+CGSN=1");
  at_log("AT+CGDCONT?");
  claim_key_load();

  if (IS_ENABLED(CONFIG_NIDD_PROBE_DEDICATED_CID)) {
    /* Set rather than assumed: a run without a dedicated context may have left
     * CID 0 Non-IP. No APN means the one the subscription names. */
    err = lte_lc_pdn_ctx_configure(0, NULL, LTE_LC_PDN_FAM_IPV4V6, NULL);
    if (err == 0) {
      err = lte_lc_pdn_ctx_create(&nidd_cid);
    }
  } else {
    err = lte_lc_pdn_default_ctx_events_enable();
  }
  if (err) {
    LOG_ERR("PDP context setup: %d", err);
    goto stop;
  }

  err = lte_lc_pdn_ctx_configure(nidd_cid, CONFIG_NIDD_PROBE_APN, LTE_LC_PDN_FAM_NONIP, NULL);
  if (err) {
    LOG_ERR("lte_lc_pdn_ctx_configure: %d", err);
    goto stop;
  }

  LOG_INF("Attaching over NB-IoT, Non-IP on CID %u, APN %s", nidd_cid, CONFIG_NIDD_PROBE_APN);
  err = lte_lc_connect_async(lte_handler);
  if (err) {
    LOG_ERR("lte_lc_connect_async: %d", err);
    goto stop;
  }

  if (k_sem_take(&registered, K_MINUTES(REGISTER_TIMEOUT_MIN)) != 0) {
    LOG_ERR("Not registered after %d minutes", REGISTER_TIMEOUT_MIN);
    at_log("AT%XICCID");
    home_network_log();
    at_log("AT+CEREG?");
    at_log("AT%XMONITOR");
    at_log("AT+CESQ");
    at_log("AT+CEER");
    goto stop;
  }

  at_log("AT%XICCID");
  home_network_log();
  at_log("AT%XMONITOR");

  if (IS_ENABLED(CONFIG_NIDD_PROBE_DEDICATED_CID)) {
    int esm = 0;

    err = lte_lc_pdn_activate(nidd_cid, &esm, NULL);
    if (err) {
      LOG_ERR("lte_lc_pdn_activate: %d, ESM %d: %s", err, esm, lte_lc_pdn_esm_strerror(esm));
      goto stop;
    }
  }

  if (k_sem_take(&pdn_up, K_SECONDS(PDN_TIMEOUT_SEC)) != 0) {
    LOG_ERR("Non-IP PDN on CID %u not active after %d s", nidd_cid, PDN_TIMEOUT_SEC);
    at_log("AT+CGACT?");
    at_log("AT+CEER");
    goto stop;
  }

  at_log("AT+CGDCONT?");
  at_log("AT+CGACT?");
  if (nidd_cid != 0) {
    at_log("AT+CGCONTRDP=0");
  }
  at_log_cid("AT+CGCONTRDP=");
  at_log_cid("AT+CGAPNRC=");
  at_log("AT+CCIOTOPT?");

  nidd_fd = socket_open();
  if (nidd_fd < 0) {
    goto stop;
  }
  LOG_INF("Ready: 'nidd raw <bytes>' sends filler, 'nidd hello' sends HELLO");

  for (;;) {
    ssize_t len = zsock_recv(nidd_fd, rx_buf, sizeof(rx_buf), 0);

    if (len < 0) {
      LOG_ERR("zsock_recv: errno %d", errno);
      break;
    }
    downlink_log(rx_buf, len);
  }

  err = nidd_fd;
  nidd_fd = -1;
  (void)zsock_close(err);

stop:
  LOG_INF("Powering the modem off");
  (void)lte_lc_power_off();

  return 0;
}
