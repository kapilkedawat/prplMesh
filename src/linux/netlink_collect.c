/*
 *  prplMesh Wi-Fi Multi-AP
 *
 *  Copyright (c) 2018, prpl Foundation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/** @todo   Review all the return codes & error handling in general ... */

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#include <netlink/attr.h>       // nla_parse()
#include <netlink/genl/genl.h>  // genlmsg_attr*()

#include "netlink_funcs.h"
#include "datamodel.h"
#include "nl80211.h"
#include "platform.h"

static int collect_protocol_features(struct nl_msg *msg, bool *splitWiphy)
{
    struct genlmsghdr   *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct nlattr       *attr;

    attr = nla_find(genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NL80211_ATTR_PROTOCOL_FEATURES);

    if ( attr ) {
        uint32_t feat = nla_get_u32(attr);

        PLATFORM_PRINTF_DEBUG_INFO("nl80211 features: 0x%x\n", feat);

        if ( feat & NL80211_PROTOCOL_FEATURE_SPLIT_WIPHY_DUMP ) {
            PLATFORM_PRINTF_DEBUG_INFO("\t* has split wiphy dump\n");
            *splitWiphy = true;
        }
    }
    return NL_SKIP;
}

/** @brief  callback to parse & collect radio attributes
 *
 *  This function is called multiple times from the netlink interface
 *  to process the different parts of the radio attributes.
 */
static int collect_radio_datas(struct nl_msg *msg, struct radio *radio)
{
    struct nlattr       *tb_msg[NL80211_ATTR_MAX + 1];
    struct genlmsghdr   *gnlh = nlmsg_data(nlmsg_hdr(msg));

    nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

    /* @todo get supported cipher suites and authentications. */

    /* How many associated stations are supported in AP mode */
    if ( tb_msg[NL80211_ATTR_MAX_AP_ASSOC_STA] ) {
        radio->maxApStations = nla_get_u32(tb_msg[NL80211_ATTR_MAX_AP_ASSOC_STA]);
    }
    /* Configured antennas */
    if ( tb_msg[NL80211_ATTR_WIPHY_ANTENNA_RX] )
        radio->confAnts[T_RADIO_RX] = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY_ANTENNA_RX]);
    if ( tb_msg[NL80211_ATTR_WIPHY_ANTENNA_TX] )
        radio->confAnts[T_RADIO_TX] = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY_ANTENNA_TX]);

    /* Supported interface modes */
    if (tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES]) {
        struct nlattr *nl_mode;
        int            rem_mode;

        nla_for_each_nested(nl_mode, tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES], rem_mode) {
            if ( nla_type(nl_mode) == NL80211_IFTYPE_MONITOR ) {
                radio->monitor = true;
                break;
            }
        }
    }
    /* Valid interface combinations */
    if ( tb_msg[NL80211_ATTR_INTERFACE_COMBINATIONS] ) {
        struct nlattr   *nl_combi;
        int              rem_combi;

        static struct nla_policy iface_combination_policy[NUM_NL80211_IFACE_COMB] = {
            [NL80211_IFACE_COMB_LIMITS] = { .type = NLA_NESTED },
            [NL80211_IFACE_COMB_MAXNUM] = { .type = NLA_U32 },
            [NL80211_IFACE_COMB_STA_AP_BI_MATCH] = { .type = NLA_FLAG },
            [NL80211_IFACE_COMB_NUM_CHANNELS] = { .type = NLA_U32 },
            [NL80211_IFACE_COMB_RADAR_DETECT_WIDTHS] = { .type = NLA_U32 },
        };
        static struct nla_policy iface_limit_policy[NUM_NL80211_IFACE_LIMIT] = {
            [NL80211_IFACE_LIMIT_TYPES] = { .type = NLA_NESTED },
            [NL80211_IFACE_LIMIT_MAX] = { .type = NLA_U32 },
        };

        nla_for_each_nested(nl_combi, tb_msg[NL80211_ATTR_INTERFACE_COMBINATIONS], rem_combi) {
            struct nlattr *tb_comb[NUM_NL80211_IFACE_COMB];
            struct nlattr *tb_limit[NUM_NL80211_IFACE_LIMIT];
            struct nlattr *nl_limit, *nl_mode;
            int            err, rem_limit, rem_mode;

            err = nla_parse_nested(tb_comb, MAX_NL80211_IFACE_COMB, nl_combi, iface_combination_policy);
            if ( err || ! (
                tb_comb[NL80211_IFACE_COMB_LIMITS] &&
                tb_comb[NL80211_IFACE_COMB_MAXNUM] &&
                tb_comb[NL80211_IFACE_COMB_NUM_CHANNELS]) ) {
                goto broken_combination;
            }

            nla_for_each_nested(nl_limit, tb_comb[NL80211_IFACE_COMB_LIMITS], rem_limit) {

                err = nla_parse_nested(tb_limit, MAX_NL80211_IFACE_LIMIT, nl_limit, iface_limit_policy);
                if ( err
                ||  ! tb_limit[NL80211_IFACE_LIMIT_TYPES] )
                    goto broken_combination;

                nla_for_each_nested(nl_mode, tb_limit[NL80211_IFACE_LIMIT_TYPES], rem_mode) {
                    /* Search for max stations per AP */
                    if ( NL80211_IFTYPE_AP == nla_type(nl_mode) ) {
                        uint32_t tmp = nla_get_u32(tb_limit[NL80211_IFACE_LIMIT_MAX]);
                        if ( radio->maxBSS < tmp )
                            radio->maxBSS = tmp;
                        break;
                    }
                }
            }
          broken_combination:;
        }
    }

    /* Bands processing */
    if ( tb_msg[NL80211_ATTR_WIPHY_BANDS] ) {
        static struct radioBand *band;
        struct nlattr           *tb_band[NL80211_BAND_ATTR_MAX + 1], *nl_band;
        int                      rem_band;

        nla_for_each_nested(nl_band, tb_msg[NL80211_ATTR_WIPHY_BANDS], rem_band) {

            if ( ! band || band->id != nl_band->nla_type ) {
                band = zmemalloc(sizeof(struct radioBand));
                PTRARRAY_ADD(radio->bands, band);
                band->id = nl_band->nla_type;
            }
            nla_parse(tb_band, NL80211_BAND_ATTR_MAX, nla_data(nl_band), nla_len(nl_band), NULL);

            /* band::ht40 */
            if ( tb_band[NL80211_BAND_ATTR_HT_CAPA] ) {
                /* Band capabilities */
                uint16_t cap = nla_get_u16(tb_band[NL80211_BAND_ATTR_HT_CAPA]);
                band->ht40 = (cap & BIT(1) == BIT(1));
            }
            /* band::channels */
            if ( tb_band[NL80211_BAND_ATTR_FREQS] ) {
                struct nlattr   *tb_freq[NL80211_FREQUENCY_ATTR_MAX + 1], *nl_freq;
                int              rem_freq;

                static struct nla_policy freq_policy[NL80211_FREQUENCY_ATTR_MAX + 1] = {
                    [NL80211_FREQUENCY_ATTR_FREQ]         = { .type = NLA_U32 },
                    [NL80211_FREQUENCY_ATTR_DISABLED]     = { .type = NLA_FLAG },
                    [NL80211_FREQUENCY_ATTR_NO_IR]        = { .type = NLA_FLAG },
                    [__NL80211_FREQUENCY_ATTR_NO_IBSS]    = { .type = NLA_FLAG },
                    [NL80211_FREQUENCY_ATTR_RADAR]        = { .type = NLA_FLAG },
                    [NL80211_FREQUENCY_ATTR_MAX_TX_POWER] = { .type = NLA_U32 },
                };

                nla_for_each_nested(nl_freq, tb_band[NL80211_BAND_ATTR_FREQS], rem_freq) {
                    struct radioChannel ch;

                    nla_parse(tb_freq, NL80211_FREQUENCY_ATTR_MAX, nla_data(nl_freq), nla_len(nl_freq), freq_policy);

                    if ( ! tb_freq[NL80211_FREQUENCY_ATTR_FREQ] )
                        continue;

                    ch.freq     = nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_FREQ]);
                    ch.id       = ieee80211_frequency_to_channel(ch.freq);
                    ch.disabled = tb_freq[NL80211_FREQUENCY_ATTR_DISABLED];
                    ch.radar    = tb_freq[NL80211_FREQUENCY_ATTR_RADAR];

                    if ( tb_freq[NL80211_FREQUENCY_ATTR_MAX_TX_POWER] )
                         ch.dbm = nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_MAX_TX_POWER]);
                    else ch.dbm = 0;

                    PTRARRAY_ADD(band->channels, ch);
                }
            }
            /* VHT Capabilities */
            if ( tb_band[NL80211_BAND_ATTR_VHT_CAPA] ) {
                uint32_t capa = nla_get_u32(tb_band[NL80211_BAND_ATTR_VHT_CAPA]);

                band->supChannelWidth = (capa >> 2) & 3;
                band->shortGI = (capa & (BIT(5) | BIT(6))) >> 5;
            }
        }
    }
    return NL_SKIP;
}

static int populate_radios_from_dev(const char *dev)
{
    char        basedir[128],
                name[T_RADIO_NAME_SZ];
    int         index;
    mac_address mac;

    sprintf(basedir, "/sys/class/net/%s/phy80211", dev);

    if ( phy_lookup(basedir, name, &mac, &index) <= 0 )
        return -1;

    radioAllocLocal(mac, name, index);
    return 0;
}

static int populate_radios_from_sysfs(void)
{
    const char      *sysfs_ieee80211_phys = "/sys/class/ieee80211";
    DIR             *d;
    struct dirent   *f;
    int              ret = 0;

    if ( ! (d = opendir(sysfs_ieee80211_phys)) )
        return -1;

    errno = 0;
    while ( (f = readdir(d)) ) {
        char        basedir[128],
                    name[T_RADIO_NAME_SZ];
        mac_address mac;
        int         index;

        if ( f->d_name[0] == '.' )  /* Skip '.', '..' & hidden files */
            continue;

        sprintf(basedir, "%s/%s", sysfs_ieee80211_phys, f->d_name);

        if ( phy_lookup(basedir, name, &mac, &index) <= 0 ) {
            ret = -1;
            break;
        }
        radioAllocLocal(mac, name, index);
        ret++;
        errno = 0;
    }
    if ( !f && errno )
        ret = -1;

    closedir(d);
    return ret;
}

int netlink_collect_local_infos(void) /* populate 'local_device' */
{
    struct nl80211_state  nlstate;
    struct radio         *radio;
    struct nl_msg        *m;
    int                   ret = 0;
    bool                  splitWiphy = false;

    if ( (ret = populate_radios_from_sysfs()) < 0 )
        return -1;
    if ( ! ret )
        return  0;
    if ( netlink_open(&nlstate) < 0 )
        return -1;

    /* Detect how the netlink protocol is to be handled */
    if ( ! (m = netlink_prepare(&nlstate, NL80211_CMD_GET_PROTOCOL_FEATURES, 0))
    ||   netlink_do(&nlstate, m, (void *)collect_protocol_features, &splitWiphy) < 0 ) {
        ret = -1;
    }
    else dlist_for_each(radio, local_device->radios, l) {
        if ( ! (m = netlink_prepare(&nlstate, NL80211_CMD_GET_WIPHY, 0)) ) {
            ret = -1;
            break;
        }
        if ( splitWiphy ) {
            nla_put_flag(m, NL80211_ATTR_SPLIT_WIPHY_DUMP);
            nlmsg_hdr(m)->nlmsg_flags |= NLM_F_DUMP;
        }
        nla_put(m, NL80211_ATTR_WIPHY, sizeof(radio->index), &radio->index);

        if ( netlink_do(&nlstate, m, (void *)collect_radio_datas, radio) < 0 ) {
            ret = -1;
            break;
        }
    }
    netlink_close(&nlstate);
    return ret;
}
