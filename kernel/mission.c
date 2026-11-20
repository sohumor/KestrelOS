#include "mission.h"
#include "proc.h"
#include "spinlock.h"
#include "string.h"
#include "pulse.h"
#include "flight.h"
#include "timer.h"

static struct {
    struct k_mission_info info;
    char exec[256];
    unsigned refs;
    struct mission_rate rate;
    int ended;
} missions[K_MISSION_MAX];
static unsigned next_id = 1;
static spinlock_t mission_lock = SPINLOCK_INIT;

static int slot(int id)
{
    for (int i = 0; i < K_MISSION_MAX; i++)
        if (missions[i].refs && missions[i].info.id == (unsigned)id) return i;
    return -1;
}

static void event(struct k_mission_info *m, unsigned kind, unsigned c,
                  const char *detail)
{
    pulse_emit(K_TOPIC_MISSION, kind, m->id, c, detail);
    if (kind == K_PULSE_MISSION_REVOKE)
        flight_record(FLIGHT_CAT_PROC, "mission", current ? current->pid : 0,
                      m->id, kind, detail);
}

static int count(int s, unsigned c, int allow, const char *detail)
{
    if (s >= 0 && mission_core_count(&missions[s].info, &missions[s].rate, c, allow,
                                    timer_ticks() * (1000 / TIMER_HZ), detail))
        event(&missions[s].info, allow ? K_PULSE_MISSION_USE : K_PULSE_MISSION_DENY,
              c, detail);
    if (s >= 0 && !allow)
        flight_record(FLIGHT_CAT_PROC, "mission", current ? current->pid : 0,
                      missions[s].info.id, K_PULSE_MISSION_DENY, detail);
    return allow;
}

int mission_create(const struct k_mission_grant *g, const char *exec)
{
    char canon[256];
    if (!mission_grant_valid(g) || mission_canon(exec, canon, sizeof(canon)) < 0)
        return -1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int parent = current ? slot(current->mission_id) : -1;
    if (current && current->mission_id && (parent < 0 ||
        !mission_grant_subset(g, &missions[parent].info.grant) ||
        !mission_core_path(&missions[parent].info, missions[parent].exec, canon, 0))) {
        spin_unlock_irqrestore(&mission_lock, f);
        return -1;
    }
    int id = -1;
    for (int s = 0; s < K_MISSION_MAX; s++) if (!missions[s].refs) {
        if (!next_id || next_id > 0x7fffffffU) break;
        memset(&missions[s], 0, sizeof(missions[s]));
        struct k_mission_info *m = &missions[s].info;
        m->id = next_id++;
        m->owner_uid = current ? current->uid : 0;
        m->grant = *g;
        if (current) m->grant.cap_mask &= current->cap_mask;
        if (!(m->grant.cap_mask & KCAP_NET)) {
            m->grant.net_mode = K_MISSION_NET_NONE;
            m->grant.host_count = 0;
        }
        if (!(m->grant.cap_mask & KCAP_IPC)) m->grant.ipc_bits = 0;
        strcpy(missions[s].exec, canon);
        missions[s].refs = 1;
        id = (int)m->id;
        event(m, K_PULSE_MISSION_START, 0, m->grant.app);
        break;
    }
    spin_unlock_irqrestore(&mission_lock, f);
    return id;
}

void mission_put(int id)
{
    if (!id) return;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(id);
    if (s >= 0 && !--missions[s].refs)
        event(&missions[s].info, K_PULSE_MISSION_END, 0, missions[s].info.grant.app);
    spin_unlock_irqrestore(&mission_lock, f);
}

void mission_inherit(struct task *child, const struct task *parent)
{
    int id = child->mission_id ? child->mission_id : (parent ? parent->mission_id : 0);
    if (!id) return;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(id);
    child->mission_id = id;
    if (s >= 0) {
        missions[s].refs++;
        child->cap_mask &= missions[s].info.grant.cap_mask;
        if (missions[s].ended) child->kill_pending = 1;
    } else child->cap_mask = 0;
    spin_unlock_irqrestore(&mission_lock, f);
}

void mission_limit_current(void)
{
    if (!current || !current->mission_id) return;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    current->cap_mask &= s >= 0 ? missions[s].info.grant.cap_mask : 0;
    spin_unlock_irqrestore(&mission_lock, f);
}

int mission_cap(uint64_t cap, int allow)
{
    if (!current || !current->mission_id) return allow;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    int c = mission_cap_channel(cap);
    allow = allow && s >= 0 && (missions[s].info.grant.cap_mask & cap);
    if (c >= 0) count(s, (unsigned)c, allow, "cap");
    spin_unlock_irqrestore(&mission_lock, f);
    return allow;
}

int mission_path(const char *path, int write)
{
    if (!current || !current->mission_id) return 1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    int allow = s >= 0 && mission_core_path(&missions[s].info, missions[s].exec, path, write);
    unsigned c = write ? K_MISSION_FS_WRITE : K_MISSION_FS_READ;
    char canon[256];
    if (mission_canon(path, canon, sizeof(canon)) == 0) {
        int d = mission_device_channel(canon);
        if (d >= 0) count(s, (unsigned)d, allow, path);
    }
    count(s, c, allow, path);
    spin_unlock_irqrestore(&mission_lock, f);
    return allow;
}

int mission_host(uint32_t ip)
{
    if (!current || !current->mission_id) return 1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    int allow = s >= 0 && mission_core_host(&missions[s].info, ip);
    char detail[40];
    struct krec_kbuf b;
    krec_kbuf_init(&b, detail, sizeof(detail));
    krec_kbuf_put_ip4(&b, ip);
    krec_kbuf_finish(&b);
    count(s, K_MISSION_NET, allow, detail);
    spin_unlock_irqrestore(&mission_lock, f);
    return allow;
}

int mission_ipc(unsigned bit)
{
    if (!current || !current->mission_id) return 1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    int allow = s >= 0 && (missions[s].info.grant.cap_mask & KCAP_IPC) &&
                (missions[s].info.grant.ipc_bits & bit);
    count(s, K_MISSION_IPC, allow, "ipc");
    spin_unlock_irqrestore(&mission_lock, f);
    return allow;
}

int mission_device(unsigned c)
{
    if (!current || !current->mission_id) return 1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(current->mission_id);
    int allow = c >= K_MISSION_AUDIO && c < K_MISSION_CHANNELS && s >= 0 &&
                (missions[s].info.grant.device_bits & (1U << (c - K_MISSION_AUDIO)));
    count(s, c, allow, "device");
    spin_unlock_irqrestore(&mission_lock, f);
    return allow;
}

int mission_ctl(int id, int op, unsigned arg)
{
    if (!current || current->mission_id) return -1;
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int s = slot(id);
    if (s < 0 || (current->uid && current->uid != missions[s].info.owner_uid)) {
        spin_unlock_irqrestore(&mission_lock, f);
        return -1;
    }
    struct k_mission_info *m = &missions[s].info;
    uint64_t drop = 0;
    int ok = 1;
    if (op == K_MISSION_REVOKE_CAP && arg < K_MISSION_CHANNELS) {
        m->revoked |= 1ULL << arg;
        drop = mission_channel_cap(arg);
        m->grant.cap_mask &= ~drop;
        if (arg >= K_MISSION_AUDIO) m->grant.device_bits &= ~(1U << (arg - K_MISSION_AUDIO));
        if (arg == K_MISSION_IPC) m->grant.ipc_bits = 0;
    } else if (op == K_MISSION_REVOKE_PATH && arg < m->grant.path_count) {
        m->grant.paths[arg].revoked = 1;
    } else if (op == K_MISSION_REVOKE_HOST && arg < m->grant.host_count) {
        m->grant.hosts[arg].revoked = 1;
    } else if (op == K_MISSION_END) {
        missions[s].ended = 1;
        m->grant.cap_mask = 0;
        m->grant.device_bits = 0;
        m->grant.ipc_bits = 0;
        m->revoked = (1ULL << K_MISSION_CHANNELS) - 1;
        drop = KCAP_ALL;
    } else ok = 0;
    if (ok) {
        unsigned channel = op == K_MISSION_END ? 0 : arg;
        const char *detail = "revoke";
        if (op == K_MISSION_REVOKE_PATH) {
            channel = m->grant.paths[arg].mode & K_MISSION_WRITE ?
                      K_MISSION_FS_WRITE : K_MISSION_FS_READ;
            detail = m->grant.paths[arg].prefix;
        } else if (op == K_MISSION_REVOKE_HOST) channel = K_MISSION_NET;
        event(m, K_PULSE_MISSION_REVOKE, channel, detail);
    }
    spin_unlock_irqrestore(&mission_lock, f);
    if (ok) proc_mission_ctl(id, drop, op == K_MISSION_END);
    return ok ? 0 : -1;
}

int mission_info(unsigned index, struct k_mission_info *out)
{
    uint64_t f = spin_lock_irqsave(&mission_lock);
    int found = 0;
    for (int s = 0; s < K_MISSION_MAX; s++) if (missions[s].refs) {
        if (index--) continue;
        *out = missions[s].info;
        found = 1;
        break;
    }
    spin_unlock_irqrestore(&mission_lock, f);
    if (found) proc_mission_pids(out);
    return found ? 0 : -1;
}

uint32_t mission_snapshot_krec(char *buf, uint32_t cap)
{
    struct krec_kbuf b;
    struct k_mission_info m;
    krec_kbuf_init(&b, buf, cap);
    krec_kbuf_puts(&b, "@krec id:int pid:int app:str mission:str granted:hex revoked:hex uses:int denies:int last_deny:str\n");
    if (b.overflow) {
        b.len = 0;
        return krec_kbuf_finish(&b);
    }
    for (unsigned i = 0; mission_info(i, &m) == 0; i++) {
        uint32_t mark = b.len;
        uint64_t uses = 0, denies = 0, granted = 0;
        for (unsigned c = 0; c < K_MISSION_CHANNELS; c++) {
            uses += m.counters[c].uses; denies += m.counters[c].denies;
            if ((m.grant.cap_mask & mission_channel_cap(c)) ||
                (c >= K_MISSION_AUDIO && (m.grant.device_bits & (1U << (c - K_MISSION_AUDIO)))))
                granted |= 1ULL << c;
        }
        krec_kbuf_put_u64(&b, m.id); krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_i64(&b, m.pid_count ? m.pids[0] : 0); krec_kbuf_putc(&b, '\t');
        krec_kbuf_escape(&b, m.grant.app); krec_kbuf_putc(&b, '\t');
        krec_kbuf_escape(&b, m.grant.title); krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_hex(&b, granted); krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_hex(&b, m.revoked); krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_u64(&b, uses); krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_u64(&b, denies); krec_kbuf_putc(&b, '\t');
        krec_kbuf_escape(&b, m.last_deny); krec_kbuf_putc(&b, '\n');
        if (b.overflow) {
            b.len = mark;
            break;
        }
    }
    return krec_kbuf_finish(&b);
}
