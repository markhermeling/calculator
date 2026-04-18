#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(garage_door, LOG_LEVEL_INF);

/* ── GPIO ───────────────────────────────────────────────────────────────── */

static const struct gpio_dt_spec relay =
	GPIO_DT_SPEC_GET(DT_NODELABEL(relay_out), gpios);
static const struct gpio_dt_spec sensor =
	GPIO_DT_SPEC_GET(DT_NODELABEL(door_sensor_in), gpios);

static bool has_sensor;

/* ── MQTT topics / payloads ─────────────────────────────────────────────── */

#define TOPIC_CMD   "garage_door/command"
#define TOPIC_STATE "garage_door/state"
#define TOPIC_AVAIL "garage_door/availability"
#define TOPIC_DISC  "homeassistant/cover/garage_door/config"

#define DISCOVERY_PAYLOAD                                        \
	"{\"name\":\"Garage Door\","                             \
	"\"unique_id\":\"pico_garage_01\","                      \
	"\"command_topic\":\"" TOPIC_CMD "\","                   \
	"\"state_topic\":\"" TOPIC_STATE "\","                   \
	"\"availability_topic\":\"" TOPIC_AVAIL "\","            \
	"\"payload_open\":\"OPEN\","                             \
	"\"payload_close\":\"CLOSE\","                           \
	"\"payload_stop\":\"STOP\","                             \
	"\"state_open\":\"open\","                               \
	"\"state_closed\":\"closed\","                           \
	"\"device\":{"                                           \
		"\"identifiers\":[\"pico_garage_01\"],"          \
		"\"name\":\"Garage Door Controller\","           \
		"\"model\":\"Raspberry Pi Pico W\","             \
		"\"manufacturer\":\"Custom\""                    \
	"}}"

/* ── MQTT client state ──────────────────────────────────────────────────── */

#define MQTT_BUF_SIZE 512

static uint8_t rx_buf[MQTT_BUF_SIZE];
static uint8_t tx_buf[MQTT_BUF_SIZE];
static uint8_t payload_buf[64];

static struct mqtt_client client;
static struct sockaddr_in broker_addr;
static int mqtt_sock = -1;

static uint16_t msg_id;
static inline uint16_t next_msg_id(void) { return ++msg_id ? msg_id : ++msg_id; }

/* ── WiFi ───────────────────────────────────────────────────────────────── */

K_SEM_DEFINE(wifi_sem, 0, 1);
K_SEM_DEFINE(ipv4_sem, 0, 1);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void on_wifi_event(struct net_mgmt_event_callback *cb,
			  uint32_t event, struct net_if *iface)
{
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *s = (const struct wifi_status *)cb->info;
		if (s->status == 0) {
			LOG_INF("WiFi connected");
			k_sem_give(&wifi_sem);
		} else {
			LOG_ERR("WiFi connect failed (%d) — rebooting", s->status);
			sys_reboot(SYS_REBOOT_COLD);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("WiFi lost — rebooting");
		sys_reboot(SYS_REBOOT_COLD);
	}
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb,
			  uint32_t event, struct net_if *iface)
{
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		LOG_INF("IPv4 address acquired");
		k_sem_give(&ipv4_sem);
	}
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_default();

	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	struct wifi_connect_req_params params = {
		.ssid        = CONFIG_GARAGE_WIFI_SSID,
		.ssid_length = strlen(CONFIG_GARAGE_WIFI_SSID),
		.psk         = CONFIG_GARAGE_WIFI_PSK,
		.psk_length  = strlen(CONFIG_GARAGE_WIFI_PSK),
		.security    = WIFI_SECURITY_TYPE_PSK,
		.channel     = WIFI_CHANNEL_ANY,
		.band        = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp         = WIFI_MFP_OPTIONAL,
	};

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
			   sizeof(params));
	if (ret) {
		LOG_ERR("WiFi connect request failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&wifi_sem, K_SECONDS(30))) {
		LOG_ERR("WiFi connect timed out");
		return -ETIMEDOUT;
	}
	if (k_sem_take(&ipv4_sem, K_SECONDS(15))) {
		LOG_ERR("DHCP timed out");
		return -ETIMEDOUT;
	}
	return 0;
}

/* ── Relay / door helpers ───────────────────────────────────────────────── */

static void pulse_relay(void)
{
	gpio_pin_set_dt(&relay, 1);
	k_msleep(CONFIG_GARAGE_PULSE_DURATION_MS);
	gpio_pin_set_dt(&relay, 0);
	LOG_INF("Relay pulsed %d ms", CONFIG_GARAGE_PULSE_DURATION_MS);
}

static const char *door_state(void)
{
	return gpio_pin_get_dt(&sensor) ? "open" : "closed";
}

/* ── MQTT publish helper ────────────────────────────────────────────────── */

static int publish(const char *topic, const char *payload, bool retain)
{
	struct mqtt_publish_param p = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = (const uint8_t *)topic,
					.size = strlen(topic),
				},
				.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			},
			.payload = {
				.data = (uint8_t *)payload,
				.len  = strlen(payload),
			},
		},
		.message_id  = next_msg_id(),
		.dup_flag    = 0,
		.retain_flag = retain ? 1 : 0,
	};
	return mqtt_publish(&client, &p);
}

/* ── MQTT event handler ─────────────────────────────────────────────────── */

static void on_mqtt_event(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	switch (evt->type) {

	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT CONNACK error %d — rebooting", evt->result);
			sys_reboot(SYS_REBOOT_COLD);
		}
		LOG_INF("MQTT connected");

		publish(TOPIC_DISC,  DISCOVERY_PAYLOAD, true);
		publish(TOPIC_AVAIL, "online",           true);
		if (has_sensor) {
			publish(TOPIC_STATE, door_state(), true);
		}

		static struct mqtt_topic sub_topic;
		static struct mqtt_subscription_list sub_list;
		sub_topic = (struct mqtt_topic){
			.topic = { .utf8 = (const uint8_t *)TOPIC_CMD,
				   .size = strlen(TOPIC_CMD) },
			.qos   = MQTT_QOS_1_AT_LEAST_ONCE,
		};
		sub_list = (struct mqtt_subscription_list){
			.list       = &sub_topic,
			.list_count = 1,
			.message_id = next_msg_id(),
		};
		mqtt_subscribe(c, &sub_list);
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_WRN("MQTT disconnected (%d) — rebooting", evt->result);
		sys_reboot(SYS_REBOOT_COLD);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;
		size_t len = MIN(p->message.payload.len, sizeof(payload_buf) - 1);

		int n = mqtt_read_publish_payload_blocking(c, payload_buf, len);
		if (n > 0) {
			payload_buf[n] = '\0';
			LOG_INF("Command received: %s", payload_buf);

			if (!strcmp((char *)payload_buf, "OPEN")  ||
			    !strcmp((char *)payload_buf, "CLOSE") ||
			    !strcmp((char *)payload_buf, "STOP")) {
				pulse_relay();
				if (has_sensor) {
					k_msleep(200);
					publish(TOPIC_STATE, door_state(), true);
				}
			}
		}

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = p->message_id,
			};
			mqtt_publish_qos1_ack(c, &ack);
		}
		break;
	}

	case MQTT_EVT_PUBACK:
	case MQTT_EVT_SUBACK:
		break;

	default:
		LOG_DBG("Unhandled MQTT event %d", evt->type);
		break;
	}
}

/* ── MQTT connect ───────────────────────────────────────────────────────── */

/* File-scope so their addresses remain valid for the session */
static struct mqtt_topic    will_topic_s;
static struct mqtt_utf8     will_msg_s;
static struct mqtt_utf8     mqtt_user_s;
static struct mqtt_utf8     mqtt_pass_s;

static int mqtt_connect_broker(void)
{
	broker_addr.sin_family = AF_INET;
	broker_addr.sin_port   = htons(CONFIG_GARAGE_MQTT_PORT);
	zsock_inet_pton(AF_INET, CONFIG_GARAGE_MQTT_BROKER,
			&broker_addr.sin_addr);

	mqtt_client_init(&client);

	/* Last will — marks device offline on unexpected disconnect */
	will_topic_s = (struct mqtt_topic){
		.topic = { .utf8 = (const uint8_t *)TOPIC_AVAIL,
			   .size = strlen(TOPIC_AVAIL) },
		.qos   = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	will_msg_s = (struct mqtt_utf8){
		.utf8 = (const uint8_t *)"offline",
		.size = strlen("offline"),
	};

	client.broker           = (struct sockaddr *)&broker_addr;
	client.evt_cb           = on_mqtt_event;
	client.client_id.utf8   = (const uint8_t *)CONFIG_GARAGE_MQTT_CLIENT_ID;
	client.client_id.size   = strlen(CONFIG_GARAGE_MQTT_CLIENT_ID);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf           = rx_buf;
	client.rx_buf_size      = sizeof(rx_buf);
	client.tx_buf           = tx_buf;
	client.tx_buf_size      = sizeof(tx_buf);
	client.transport.type   = MQTT_TRANSPORT_NON_SECURE;
	client.will_topic       = &will_topic_s;
	client.will_message     = &will_msg_s;
	client.will_retain      = 1;

	if (strlen(CONFIG_GARAGE_MQTT_USER) > 0) {
		mqtt_user_s = (struct mqtt_utf8){
			.utf8 = (const uint8_t *)CONFIG_GARAGE_MQTT_USER,
			.size = strlen(CONFIG_GARAGE_MQTT_USER),
		};
		mqtt_pass_s = (struct mqtt_utf8){
			.utf8 = (const uint8_t *)CONFIG_GARAGE_MQTT_PASSWORD,
			.size = strlen(CONFIG_GARAGE_MQTT_PASSWORD),
		};
		client.user_name = &mqtt_user_s;
		client.password  = &mqtt_pass_s;
	}

	int ret = mqtt_connect(&client);
	if (ret) {
		LOG_ERR("mqtt_connect failed: %d", ret);
		return ret;
	}

	mqtt_sock = client.transport.tcp.sock;
	return 0;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
	LOG_INF("Garage door controller starting");

	/* Relay output */
	if (!device_is_ready(relay.port)) {
		LOG_ERR("Relay GPIO not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE);

	/* Optional door sensor */
	has_sensor = device_is_ready(sensor.port);
	if (has_sensor) {
		gpio_pin_configure_dt(&sensor, GPIO_INPUT);
		LOG_INF("Door sensor enabled");
	} else {
		LOG_WRN("Door sensor unavailable — state reporting disabled");
	}

	if (wifi_connect() != 0) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	if (mqtt_connect_broker() != 0) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	struct zsock_pollfd fds = {
		.fd     = mqtt_sock,
		.events = ZSOCK_POLLIN,
	};

	while (true) {
		int rc = zsock_poll(&fds, 1, 30 * MSEC_PER_SEC);
		if (rc < 0) {
			LOG_ERR("poll error %d — rebooting", errno);
			sys_reboot(SYS_REBOOT_COLD);
		}
		if (rc > 0) {
			if (fds.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
				LOG_WRN("Socket closed — rebooting");
				sys_reboot(SYS_REBOOT_COLD);
			}
			if (fds.revents & ZSOCK_POLLIN) {
				mqtt_input(&client);
			}
		}
		/* Sends PINGREQ when keepalive interval has elapsed */
		mqtt_live(&client);
	}

	return 0;
}
