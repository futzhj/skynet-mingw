/* mhcrypt —— 621 线路密码学的 Lua C 模块（迁移任务 P0-07 / P0-W1）
 *
 * 权威实现是 Python 侧的 server/local_server_tool/tools/local_mhxy_server/codec.py。
 * 本文件是它的逐字节等价物，验收面是 server/local_server_tool/vectors/621/L0_crypto.ndjson
 * 的 217 条向量（9 个 op）。**任何一字节不一致都算失败**，不存在"语义等价即可"。
 *
 * 与 codec.py 的对应：
 *   Rc4State            -> rc4 userdata（mhcrypt.rc4 / rc4_from_key / rc4_from_snapshot）
 *   shuffle             -> 内联在 build_table()
 *   build_exchange_table-> mhcrypt.exchange_table
 *   table_index         -> table_index()
 *   byte_position       -> mhcrypt.byte_position
 *   decode_body         -> mhcrypt.decode_body
 *   encode_body         -> mhcrypt.encode_body
 *   build_data_stream   -> mhcrypt.data_stream
 *   data_crypt          -> rc4:crypt
 *
 * 交换表是一个 userdata（不是 Lua string，也不是 Lua table）：内部就是 2079 个字节，
 * 对外给 :get(i)（0 基）、:tostring()（原始字节，好和向量的 expect_hex 逐字节比）和 #t。
 * 这个形状是既定契约，由 test/l0_common.lua:178-195 与 test/l0_smoke.lua:20-22 消费。
 * 做成 userdata 而不是 string 还顺带消掉一类漏洞：表项值域由构造保证，
 * encode_body/decode_body 拿到它就不必再防"调用方塞进越界字节"。
 */

#include <lua.h>
#include <lauxlib.h>

#include <string.h>

/* codec.py 的 MAX_LENGTH。table_len = (64+1)*64/2 - 1 = 2079，
 * 而 sum(block for block in 2..64) 也是 2079 —— 两者必须相等，
 * 否则 build_table 的分块正好铺不满整张表。下面的编译期断言钉住这条。 */
#define MH_MAX_LENGTH 64
#define MH_TABLE_LEN  2079

typedef char mh_table_len_check[((MH_MAX_LENGTH + 1) * MH_MAX_LENGTH / 2 - 1 == MH_TABLE_LEN) ? 1 : -1];

/* 换位下标全程走 int，所以把包体长度收敛到远小于 INT_MAX 的上限，
 * 免得 size_t -> int 的窄化在 64 位下变成实现定义行为。
 * 真实包体是几十到几千字节量级，这个上限碰不到。keystream 的 count 共用它。 */
#define MH_MAX_BODY (1 << 24)

#define MH_RC4_MT   "mhcrypt.rc4"
#define MH_TABLE_MT "mhcrypt.table"

typedef struct {
    unsigned char s[256];
    int i;
    int j;
} mh_rc4;

typedef struct {
    unsigned char t[MH_TABLE_LEN];
} mh_table;

/* ---------------------------------------------------------------- RC4 */

static void
rc4_init_key(mh_rc4 *rc, const unsigned char *key, size_t key_len) {
    int i;
    int j = 0;
    for (i = 0; i < 256; i++) {
        rc->s[i] = (unsigned char)i;
    }
    for (i = 0; i < 256; i++) {
        unsigned char tmp;
        j = (j + rc->s[i] + key[i % key_len]) & 0xFF;
        tmp = rc->s[i];
        rc->s[i] = rc->s[j];
        rc->s[j] = tmp;
    }
    rc->i = 0;
    rc->j = 0;
}

static void
rc4_init_seed(mh_rc4 *rc, lua_Unsigned seed) {
    unsigned char key[4];
    /* 小端四字节 —— codec.py: [seed&0xFF, (seed>>8)&0xFF, (seed>>16)&0xFF, (seed>>24)&0xFF] */
    key[0] = (unsigned char)(seed & 0xFF);
    key[1] = (unsigned char)((seed >> 8) & 0xFF);
    key[2] = (unsigned char)((seed >> 16) & 0xFF);
    key[3] = (unsigned char)((seed >> 24) & 0xFF);
    rc4_init_key(rc, key, 4);
}

static unsigned char
rc4_next(mh_rc4 *rc) {
    unsigned char tmp;
    rc->i = (rc->i + 1) & 0xFF;
    rc->j = (rc->j + rc->s[rc->i]) & 0xFF;
    tmp = rc->s[rc->i];
    rc->s[rc->i] = rc->s[rc->j];
    rc->s[rc->j] = tmp;
    return rc->s[(rc->s[rc->j] + rc->s[rc->i]) & 0xFF];
}

static mh_rc4 *
rc4_check(lua_State *L, int idx) {
    return (mh_rc4 *)luaL_checkudata(L, idx, MH_RC4_MT);
}

static mh_rc4 *
rc4_push_new(lua_State *L) {
    mh_rc4 *rc = (mh_rc4 *)lua_newuserdatauv(L, sizeof(mh_rc4), 0);
    luaL_setmetatable(L, MH_RC4_MT);
    return rc;
}

/* seed 用 lua_Integer 收，再截到 32 位：Lua 侧写 0xEDB0A703 是正数，
 * 但调用方也可能传已经溢出成负数的值，两种都要接受。 */
static lua_Unsigned
check_seed(lua_State *L, int idx) {
    return (lua_Unsigned)(luaL_checkinteger(L, idx)) & 0xFFFFFFFFu;
}

static int
l_rc4_new(lua_State *L) {
    /* 先取参再建对象：checkinteger 会 longjmp，放在后面的话栈上会先多一个半成品。 */
    lua_Unsigned seed = check_seed(L, 1);
    mh_rc4 *rc = rc4_push_new(L);
    rc4_init_seed(rc, seed);
    return 1;
}

static int
l_rc4_from_key(lua_State *L) {
    size_t len = 0;
    const char *key = luaL_checklstring(L, 1, &len);
    mh_rc4 *rc;
    /* codec.py 在空 key 上 raise ValueError，这里也必须拒绝：
     * 放过去的话 i % key_len 会除零。 */
    luaL_argcheck(L, len > 0, 1, "RC4 key must not be empty");
    rc = rc4_push_new(L);
    rc4_init_key(rc, (const unsigned char *)key, len);
    return 1;
}

static int
l_rc4_from_snapshot(lua_State *L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    lua_Integer j = luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *sbox = luaL_checklstring(L, 3, &len);
    mh_rc4 *rc;
    luaL_argcheck(L, len == 256, 3, "RC4 snapshot S-box must contain 256 bytes");
    rc = rc4_push_new(L);
    memcpy(rc->s, sbox, 256);
    rc->i = (int)(i & 0xFF);
    rc->j = (int)(j & 0xFF);
    return 1;
}

static int
l_rc4_next_byte(lua_State *L) {
    mh_rc4 *rc = rc4_check(L, 1);
    lua_pushinteger(L, (lua_Integer)rc4_next(rc));
    return 1;
}

static int
l_rc4_crypt(lua_State *L) {
    mh_rc4 *rc = rc4_check(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len);
    size_t k;
    for (k = 0; k < len; k++) {
        out[k] = (char)((unsigned char)data[k] ^ rc4_next(rc));
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

static int
l_rc4_keystream(lua_State *L) {
    mh_rc4 *rc = rc4_check(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);
    luaL_Buffer b;
    char *out;
    lua_Integer k;
    /* 上限与包体一致：没有上限的话 keystream(2^62) 会先试着分配 4 EB，
     * 在 lua_Integer 是 64 位、size_t 是 64 位的平台上表现为一次巨大分配失败，
     * 在窄化的平台上则会分配一小块然后越界写满 count 次。 */
    luaL_argcheck(L, count >= 0 && count <= MH_MAX_BODY, 2, "count out of range");
    out = luaL_buffinitsize(L, &b, (size_t)count);
    for (k = 0; k < count; k++) {
        out[k] = (char)rc4_next(rc);
    }
    luaL_pushresultsize(&b, (size_t)count);
    return 1;
}

static int
l_rc4_snapshot(lua_State *L) {
    mh_rc4 *rc = rc4_check(L, 1);
    lua_pushinteger(L, rc->i);
    lua_pushinteger(L, rc->j);
    lua_pushlstring(L, (const char *)rc->s, 256);
    return 3;
}

static int
l_rc4_clone(lua_State *L) {
    mh_rc4 *src = rc4_check(L, 1);
    mh_rc4 *dst = rc4_push_new(L);
    *dst = *src;
    return 1;
}

/* -------------------------------------------------------- 交换表 */

/* codec.py: (2 + block - 1) * (block - 2) // 2 + pos */
static int
table_index(int block, int pos) {
    return (block + 1) * (block - 2) / 2 + pos;
}

/* codec.py 的 shuffle + build_exchange_table 合并。rc 会被推进 —— 这是
 * build_data_stream 依赖的副作用，不是顺带的：数据流必须从表建完之后接着走。 */
static void
build_table(mh_rc4 *rc, unsigned char *table, int invert) {
    unsigned char raw[MH_TABLE_LEN];
    int offset = 0;
    int block;

    for (block = 2; block <= MH_MAX_LENGTH; block++) {
        int i;
        for (i = 0; i < block; i++) {
            raw[offset + i] = (unsigned char)i;
        }
        for (i = 0; i < block - 1; i++) {
            int r = rc4_next(rc) % (block - i);
            int last = block - i - 1;
            if (r != last) {
                unsigned char tmp = raw[offset + r];
                raw[offset + r] = raw[offset + last];
                raw[offset + last] = tmp;
            }
        }
        offset += block;
    }

    if (!invert) {
        memcpy(table, raw, MH_TABLE_LEN);
        return;
    }

    offset = 0;
    for (block = 2; block <= MH_MAX_LENGTH; block++) {
        int j;
        for (j = 0; j < block; j++) {
            table[offset + raw[offset + j]] = (unsigned char)j;
        }
        offset += block;
    }
}

static int
l_build_exchange_table(lua_State *L) {
    int invert = lua_toboolean(L, 2);
    mh_rc4 local;
    mh_rc4 *rc;
    mh_table *out;

    /* 第三参给了 rc4 就用它（并推进它），没给就按 seed 现造一个 ——
     * 与 codec.py 的 build_exchange_table(seed, invert, rc=None) 同构。
     * 给了 rc 时 seed 必须被完全忽略（连校验都不做）：test/l0_common.lua 的
     * count_consumption 就是靠 build_exchange_table(0, false, a) 来钉这条的。 */
    if (lua_isnoneornil(L, 3)) {
        rc4_init_seed(&local, check_seed(L, 1));
        rc = &local;
    } else {
        rc = rc4_check(L, 3);
    }

    /* 先算完再建 userdata：build_table 不碰 Lua 栈，但把分配放在校验之后，
     * 参数错时不会先在栈上留一个半成品对象。 */
    out = (mh_table *)lua_newuserdatauv(L, sizeof(mh_table), 0);
    luaL_setmetatable(L, MH_TABLE_MT);
    build_table(rc, out->t, invert);
    return 1;
}

static mh_table *
table_check(lua_State *L, int idx) {
    return (mh_table *)luaL_checkudata(L, idx, MH_TABLE_MT);
}

static int
l_table_get(lua_State *L) {
    mh_table *t = table_check(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    /* 0 基 —— l0_common.lua:178 是 t:get(i - 1)，l0_smoke.lua:20 是 for i = 0, #t - 1 */
    luaL_argcheck(L, i >= 0 && i < MH_TABLE_LEN, 2, "index out of range");
    lua_pushinteger(L, (lua_Integer)t->t[i]);
    return 1;
}

static int
l_table_tostring(lua_State *L) {
    mh_table *t = table_check(L, 1);
    lua_pushlstring(L, (const char *)t->t, MH_TABLE_LEN);
    return 1;
}

static int
l_table_len(lua_State *L) {
    table_check(L, 1);
    lua_pushinteger(L, MH_TABLE_LEN);
    return 1;
}

static int
l_data_stream(lua_State *L) {
    unsigned char scratch[MH_TABLE_LEN];
    lua_Unsigned seed = check_seed(L, 1);
    mh_rc4 *rc = rc4_push_new(L);
    rc4_init_seed(rc, seed);
    /* codec.py 的 build_data_stream：造表只为把流推进到正确位置，表本身丢弃。 */
    build_table(rc, scratch, 1);
    return 1;
}

/* -------------------------------------------------------- 换位 */

/* 换位函数一律收 mhcrypt.table userdata。表项值域由 build_table 保证在 [0, block)，
 * 所以这里不再需要逐项校验 —— 换成 string 表示的话就必须校验，否则调用方塞一个
 * 越界字节进来就能让 encode_body 往缓冲区外写。 */
static const unsigned char *
check_table(lua_State *L, int idx) {
    return table_check(L, idx)->t;
}

static int
check_body_len(lua_State *L, int idx, size_t len) {
    luaL_argcheck(L, len <= MH_MAX_BODY, idx, "body too long for the transposition table");
    return (int)len;
}

static int
byte_position(int length, int pos, const unsigned char *table) {
    int start = (pos / MH_MAX_LENGTH) * MH_MAX_LENGTH;
    int block = length - start;
    int inner = pos % MH_MAX_LENGTH;
    if (block > MH_MAX_LENGTH) {
        block = MH_MAX_LENGTH;
    }
    if (block <= 1) {
        return start;
    }
    return start + table[table_index(block, inner)];
}

static int
l_byte_position(lua_State *L) {
    lua_Integer length = luaL_checkinteger(L, 1);
    lua_Integer pos = luaL_checkinteger(L, 2);
    const unsigned char *table = check_table(L, 3);
    luaL_argcheck(L, length >= 0 && length <= MH_MAX_BODY, 1, "length out of range");
    luaL_argcheck(L, pos >= 0 && pos < length, 2, "pos out of range");
    lua_pushinteger(L, byte_position((int)length, (int)pos, table));
    return 1;
}

static int
l_decode_body(lua_State *L) {
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    const unsigned char *table = check_table(L, 2);
    int n = check_body_len(L, 1, len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len);
    int p;
    for (p = 0; p < n; p++) {
        out[p] = data[byte_position(n, p, table)];
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

static int
l_encode_body(lua_State *L) {
    size_t len = 0;
    const char *plain = luaL_checklstring(L, 1, &len);
    const unsigned char *table = check_table(L, 2);
    int n = check_body_len(L, 1, len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len);
    int p;
    /* byte_position 在每个分块上是双射，所以每个输出位置都会被写到一次 ——
     * 但 luaL_buffinitsize 给的是未初始化内存，而 codec.py 那边是 bytearray(n)（零填充）。
     * 万一将来表的构造变了、双射性被破坏，没有这次 memset 就会把堆上的残留字节
     * 当成密文发出去。代价是一次 memset，留着。 */
    memset(out, 0, len);
    for (p = 0; p < n; p++) {
        out[byte_position(n, p, table)] = plain[p];
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

/* -------------------------------------------------------- ABI 自检 */

/* 每个 Lua C 模块在这套构建里都**静态私链一份 Lua**（Makefile: LUA_LIB ?= LUA_STATICLIB）。
 * 因此本文件编译期看到的 LUA_VERSION_NUM 是模块自己那份头文件的值，不是宿主的 ——
 * 单报编译期常量会把「模块和宿主对不上」这件事恰好藏起来。
 * 所以两边都报：宿主侧从传入的 L 上取 _VERSION 与 string.packsize("j")，
 * 模块侧报编译期常量，对不上时 match=false。
 */
/* 从宿主 state 上实测 lua_Integer 宽度：string.packsize("j")。
 * 拿不到（string 库没开、packsize 不存在）时返回 -1。栈保持平衡。 */
static int
host_integer_size(lua_State *L) {
    int packsize = -1;
    lua_getglobal(L, "string");
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_getfield(L, -1, "packsize");
        if (lua_type(L, -1) == LUA_TFUNCTION) {
            lua_pushliteral(L, "j");
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isinteger(L, -1)) {
                packsize = (int)lua_tointeger(L, -1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return packsize;
}

static int
l_abi(lua_State *L) {
    int packsize = -1;
    const char *host_version = "?";

    lua_getglobal(L, "_VERSION");
    if (lua_type(L, -1) == LUA_TSTRING) {
        host_version = lua_tostring(L, -1);
    }
    /* 留在栈上：host_version 指向它，pop 掉就可能被回收 */

    packsize = host_integer_size(L);

    lua_pushstring(L, host_version);
    lua_pushinteger(L, LUA_VERSION_NUM);
    lua_pushinteger(L, packsize);
    lua_pushinteger(L, (lua_Integer)sizeof(lua_Integer));
    /* packsize 是宿主实测的 lua_Integer 宽度，sizeof 是模块编译期的。
     * 两者不等就说明模块和宿主的 Lua 配置不同，此时任何整数往返都不可信。 */
    lua_pushboolean(L, packsize == (int)sizeof(lua_Integer));
    return 5;
}

/* -------------------------------------------------------- 注册 */

/* 方法名是既定契约，由 test/l0_common.lua:178-195 与 test/l0_smoke.lua:20-22 消费。 */
static const luaL_Reg table_methods[] = {
    {"get",      l_table_get},
    {"tostring", l_table_tostring},
    {NULL, NULL},
};

static const luaL_Reg rc4_methods[] = {
    {"next_byte", l_rc4_next_byte},
    {"crypt",     l_rc4_crypt},
    {"keystream", l_rc4_keystream},
    {"snapshot",  l_rc4_snapshot},
    {"clone",     l_rc4_clone},
    {NULL, NULL},
};

/* 函数名是既定契约，由 test/l0_common.lua 与 test/l0_smoke.lua 消费，不要改。 */
static const luaL_Reg mhcrypt_funcs[] = {
    {"rc4_new",              l_rc4_new},
    {"rc4_from_key",         l_rc4_from_key},
    {"rc4_from_snapshot",    l_rc4_from_snapshot},
    {"build_exchange_table", l_build_exchange_table},
    {"data_stream",          l_data_stream},
    {"byte_position",        l_byte_position},
    {"decode_body",          l_decode_body},
    {"encode_body",          l_encode_body},
    {"abi",                  l_abi},
    {NULL, NULL},
};

int
luaopen_mhcrypt(lua_State *L) {
    int host_isize;

    luaL_checkversion(L);

    /* luaL_checkversion 在这套构建里靠不住：每个模块静态私链一份 Lua
     * （Makefile: LUA_LIB ?= LUA_STATICLIB），它比的是模块自己那份头文件的常量。
     * 真正能发现"模块和宿主的 Lua 配置不同"的，是拿宿主实测的 lua_Integer 宽度
     * 和模块编译期的 sizeof 对一下。不一致时**当场拒绝加载** ——
     * 放进 abi() 的某个返回值里没用，l0_runner.lua 只取前三个，没人会看第五个。 */
    host_isize = host_integer_size(L);
    if (host_isize > 0 && host_isize != (int)sizeof(lua_Integer)) {
        return luaL_error(L,
            "mhcrypt ABI mismatch: host lua_Integer is %d bytes, module was built for %d",
            host_isize, (int)sizeof(lua_Integer));
    }

    luaL_newmetatable(L, MH_RC4_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, rc4_methods, 0);
    lua_pushliteral(L, MH_RC4_MT);
    lua_setfield(L, -2, "__name");
    lua_pop(L, 1);

    /* 交换表的元表：__index 指回自身当方法表，另挂 __len 让 l0_smoke.lua 的 #t 可用。 */
    luaL_newmetatable(L, MH_TABLE_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, table_methods, 0);
    lua_pushcfunction(L, l_table_len);
    lua_setfield(L, -2, "__len");
    lua_pushliteral(L, MH_TABLE_MT);
    lua_setfield(L, -2, "__name");
    lua_pop(L, 1);

    luaL_newlib(L, mhcrypt_funcs);
    lua_pushinteger(L, MH_MAX_LENGTH);
    lua_setfield(L, -2, "MAX_LENGTH");
    lua_pushinteger(L, MH_TABLE_LEN);
    lua_setfield(L, -2, "TABLE_LEN");
    return 1;
}
