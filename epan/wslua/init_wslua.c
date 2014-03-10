/*
 * init_wslua.c
 *
 * Wireshark's interface to the Lua Programming Language
 *
 * (c) 2006, Luis E. Garcia Ontanon <luis@ontanon.org>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include "wslua.h"
#include "init_wslua.h"
#include <epan/dissectors/packet-frame.h>
#include <math.h>
#include <epan/expert.h>
#include <epan/ex-opt.h>
#include <wsutil/privileges.h>
#include <wsutil/file_util.h>

/* linked list of Lua plugins */
typedef struct _wslua_plugin {
    gchar       *name;            /**< plugin name */
    gchar       *version;         /**< plugin version */
    gchar       *filename;        /**< plugin filename */
    struct _wslua_plugin *next;
} wslua_plugin;

static wslua_plugin *wslua_plugin_list = NULL;

static lua_State* L = NULL;

/* XXX: global variables? Really?? Yuck. These could be done differently,
   using the Lua registry */
packet_info* lua_pinfo;
struct _wslua_treeitem* lua_tree;
tvbuff_t* lua_tvb;
int lua_dissectors_table_ref = LUA_NOREF;
int lua_heur_dissectors_table_ref = LUA_NOREF;

static int proto_lua = -1;
static expert_field ei_lua_error = EI_INIT;

dissector_handle_t lua_data_handle;

static void lua_frame_end(void)
{
    clear_outstanding_Tvb();
    clear_outstanding_TvbRange();
    clear_outstanding_Pinfo();
    clear_outstanding_Column();
    clear_outstanding_Columns();
    clear_outstanding_PrivateTable();
    clear_outstanding_TreeItem();
    clear_outstanding_FieldInfo();
}

static int wslua_not_register_menu(lua_State* LS) {
    luaL_error(LS,"too late to register a menu");
    return 0;
}

int dissect_lua(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree, void* data _U_) {
    int consumed_bytes = tvb_length(tvb);
    lua_pinfo = pinfo;
    lua_tvb = tvb;

    lua_tree = (struct _wslua_treeitem *)g_malloc(sizeof(struct _wslua_treeitem));
    lua_tree->tree = tree;
    lua_tree->item = proto_tree_add_text(tree,tvb,0,0,"lua fake item");
    lua_tree->expired = FALSE;
    PROTO_ITEM_SET_HIDDEN(lua_tree->item);

    /*
     * almost equivalent to Lua:
     * dissectors[current_proto](tvb,pinfo,tree)
     */

    lua_settop(L,0);

    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_dissectors_table_ref);

    lua_pushstring(L, pinfo->current_proto);
    lua_gettable(L, -2);

    lua_remove(L,1);


    if (lua_isfunction(L,1)) {

        push_Tvb(L,tvb);
        push_Pinfo(L,pinfo);
        push_TreeItem(L,lua_tree);

        if  ( lua_pcall(L,3,1,0) ) {
            proto_tree_add_expert_format(tree, pinfo, &ei_lua_error, tvb, 0, 0, "Lua Error: %s", lua_tostring(L,-1));
        } else {

            /* if the Lua dissector reported the consumed bytes, pass it to our caller */
            if (lua_isnumber(L, -1)) {
                /* we got the consumed bytes or the missing bytes as a negative number */
                consumed_bytes = wslua_togint(L, -1);
                lua_pop(L, 1);
            }
        }

    } else {
        proto_tree_add_expert_format(tree, pinfo, &ei_lua_error, tvb, 0, 0,
                    "Lua Error: did not find the %s dissector in the dissectors table", pinfo->current_proto);
    }

    register_frame_end_routine(pinfo, lua_frame_end);

    lua_pinfo = NULL;
    lua_tree = NULL;
    lua_tvb = NULL;

    return consumed_bytes;

}

/** Type of a heuristic dissector, used in heur_dissector_add().
 *
 * @param tvb the tvbuff with the (remaining) packet data
 * @param pinfo the packet info of this packet (additional info)
 * @param tree the protocol tree to be build or NULL
 * @return TRUE if the packet was recognized by the sub-dissector (stop dissection here)
 */
gboolean heur_dissect_lua(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree, void* data _U_) {
    gboolean result = FALSE;
    lua_tvb = tvb;
    lua_pinfo = pinfo;

    if (!tvb || !pinfo || !pinfo->heur_list_name || !pinfo->current_proto) {
        report_failure("internal error in heur_dissect_lua: NULL packet info");
        return FALSE;
    }

    /* heuristic functions are stored in a table in the registry; the registry has a
     * table at reference lua_heur_dissectors_table_ref, and that table has keys for
     * the heuristic listname (e.g., "udp", "tcp", etc.), and that key's value is a
     * table of keys of the Proto->name, and their value is the function.
     * So it's like registry[table_ref][heur_list_name][proto_name] = func
     */

    lua_settop(L,0);

    /* get the table of all lua heuristic dissector lists */
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_heur_dissectors_table_ref);

    /* get the table inside that, for the lua heuristic dissectors of the requested heur list */
    if (!wslua_get_table(L, -1, pinfo->heur_list_name)) {
        /* this shouldn't happen */
        lua_settop(L,0);
        report_failure("internal error in heur_dissect_lua: no %s heur list table", pinfo->heur_list_name);
        return FALSE;
    }

    /* get the table inside that, for the specific lua heuristic dissector */
    if (!wslua_get_field(L,-1,pinfo->current_proto)) {
        /* this shouldn't happen */
        lua_settop(L,0);
        report_failure("internal error in heur_dissect_lua: no %s heuristic dissector for list %s",
                        pinfo->current_proto, pinfo->heur_list_name);
        return FALSE;
    }

    /* remove the table of all lists (the one in the registry) */
    lua_remove(L,1);
    /* remove the heur_list_name heur list table */
    lua_remove(L,1);

    if (!lua_isfunction(L,-1)) {
        /* this shouldn't happen */
        lua_settop(L,0);
        report_failure("internal error in heur_dissect_lua: %s heuristic dissector is not a function", pinfo->current_proto);
        return FALSE;
    }

    lua_tree = (struct _wslua_treeitem *)g_malloc(sizeof(struct _wslua_treeitem));
    lua_tree->tree = tree;
    lua_tree->item = proto_tree_add_text(tree,tvb,0,0,"lua fake item");
    lua_tree->expired = FALSE;
    PROTO_ITEM_SET_HIDDEN(lua_tree->item);

    push_Tvb(L,tvb);
    push_Pinfo(L,pinfo);
    push_TreeItem(L,lua_tree);

    if  ( lua_pcall(L,3,1,0) ) {
        report_failure(" error calling %s heuristic dissector: %s", pinfo->current_proto, lua_tostring(L,-1));
        lua_settop(L,0);
    } else {
        if (lua_isboolean(L, -1) || lua_isnil(L, -1)) {
            result = lua_toboolean(L, -1);
        } else {
            report_failure(" invalid return value from Lua %s heuristic dissector", pinfo->current_proto);
        }
        lua_pop(L, 1);
    }

    register_frame_end_routine(pinfo, lua_frame_end);

    lua_pinfo = NULL;
    lua_tree = NULL;
    lua_tvb = NULL;

    return result;
}

static void iter_table_and_call(lua_State* LS, const gchar* table_name, lua_CFunction error_handler) {
    lua_settop(LS,0);

    lua_pushcfunction(LS,error_handler);
    lua_getglobal(LS, table_name);

    if (!lua_istable(LS, 2)) {
        report_failure("Lua: either `%s' does not exist or it is not a table!\n",table_name);
        lua_close(LS);
        L = NULL;
        return;
    }

    lua_pushnil(LS);

    while (lua_next(LS, 2)) {
        const gchar* name = lua_tostring(L,-2);

        if (lua_isfunction(LS,-1)) {

            if ( lua_pcall(LS,0,0,1) ) {
                    lua_pop(LS,1);
            }

        } else {
            report_failure("Lua: Something not a function got its way into the %s.%s",table_name,name);
            lua_close(LS);
            L = NULL;
            return;
        }
    }

    lua_settop(LS,0);
}


static int init_error_handler(lua_State* LS) {
    const gchar* error =  lua_tostring(LS,1);
    report_failure("Lua: Error During execution of Initialization:\n %s",error);
    return 0;
}


static void wslua_init_routine(void) {
    static gboolean initialized = FALSE;

    if ( ! initialized ) {
        lua_prime_all_fields(NULL);
        initialized = TRUE;
    }

    if (L) {
        iter_table_and_call(L, WSLUA_INIT_ROUTINES,init_error_handler);
    }

}

static int prefs_changed_error_handler(lua_State* LS) {
    const gchar* error =  lua_tostring(LS,1);
    report_failure("Lua: Error During execution of prefs apply callback:\n %s",error);
    return 0;
}

void wslua_prefs_changed(void) {
    if (L) {
        iter_table_and_call(L, WSLUA_PREFS_CHANGED,prefs_changed_error_handler);
    }
}

static const char *getF(lua_State *LS _U_, void *ud, size_t *size)
{
    FILE *f=(FILE *)ud;
    static char buff[512];
    if (feof(f)) return NULL;
    *size=fread(buff,1,sizeof(buff),f);
    return (*size>0) ? buff : NULL;
}

static int lua_main_error_handler(lua_State* LS) {
    const gchar* error =  lua_tostring(LS,1);
    report_failure("Lua: Error during loading:\n %s",error);
    return 0;
}

static void wslua_add_plugin(gchar *name, gchar *version, gchar *filename)
{
    wslua_plugin *new_plug, *lua_plug;

    lua_plug = wslua_plugin_list;
    new_plug = (wslua_plugin *)g_malloc(sizeof(wslua_plugin));

    if (!lua_plug) { /* the list is empty */
        wslua_plugin_list = new_plug;
    } else {
        while (lua_plug->next != NULL) {
            lua_plug = lua_plug->next;
        }
        lua_plug->next = new_plug;
    }

    new_plug->name = name;
    new_plug->version = version;
    new_plug->filename = filename;
    new_plug->next = NULL;
}

static int lua_script_push_args(const int script_num) {
    gchar* argname = g_strdup_printf("lua_script%d", script_num);
    const gchar* argvalue = NULL;
    int count = 0;

    while((argvalue = ex_opt_get_next(argname))) {
        lua_pushstring(L,argvalue);
        count++;
    }

    g_free(argname);
    return count;
}

/* If file_count > 0 then it's a command-line-added user script, and the count
 * represents which user script it is (first=1, second=2, etc.).
 */
static gboolean lua_load_script(const gchar* filename, const int file_count) {
    FILE* file;
    int error;
    int numargs = 0;

    if (! ( file = ws_fopen(filename,"r")) ) {
        report_open_failure(filename,errno,FALSE);
        return FALSE;
    }

    lua_settop(L,0);

    lua_pushcfunction(L,lua_main_error_handler);

#if LUA_VERSION_NUM >= 502
    error = lua_load(L,getF,file,filename,NULL);
#else
    error = lua_load(L,getF,file,filename);
#endif
    switch (error) {
        case 0:
            if (file_count > 0) {
                numargs = lua_script_push_args(file_count);
            }
            lua_pcall(L,numargs,0,1);
            fclose(file);
            lua_pop(L,1); /* pop the error handler */
            return TRUE;
        case LUA_ERRSYNTAX: {
            report_failure("Lua: syntax error during precompilation of `%s':\n%s",filename,lua_tostring(L,-1));
            fclose(file);
            return FALSE;
        }
        case LUA_ERRMEM:
            report_failure("Lua: memory allocation error during execution of %s",filename);
            fclose(file);
            return FALSE;
        default:
            report_failure("Lua: unknown error during execution of %s: %d",filename,error);
            fclose(file);
            return FALSE;
    }
}

static void basic_logger(const gchar *log_domain _U_,
                          GLogLevelFlags log_level _U_,
                          const gchar *message,
                          gpointer user_data _U_) {
    fputs(message,stderr);
}

static int wslua_panic(lua_State* LS) {
    g_error("LUA PANIC: %s",lua_tostring(LS,-1));
    /** g_error() does an abort() and thus never returns **/
    return 0; /* keep gcc happy */
}

static int lua_load_plugins(const char *dirname, register_cb cb, gpointer client_data, gboolean count_only) {
    WS_DIR        *dir;             /* scanned directory */
    WS_DIRENT     *file;            /* current file */
    gchar         *filename, *dot;
    const gchar   *name;
    int            plugins_counter = 0;

    if ((dir = ws_dir_open(dirname, 0, NULL)) != NULL) {
        while ((file = ws_dir_read_name(dir)) != NULL) {
            name = ws_dir_get_name(file);

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;        /* skip "." and ".." */

            filename = g_strdup_printf("%s" G_DIR_SEPARATOR_S "%s", dirname, name);
            if (test_for_directory(filename) == EISDIR) {
                plugins_counter += lua_load_plugins(filename, cb, client_data, count_only);
                g_free(filename);
                continue;
            }

            /* skip files starting wih . */
            if (name[0] == '.') {
                g_free(filename);
                continue;
            }

            /* skip anything but files with .lua suffix */
            dot = strrchr(name, '.');
            if (dot == NULL || g_ascii_strcasecmp(dot+1, "lua") != 0) {
                g_free(filename);
                continue;
            }

            if (file_exists(filename)) {
                if (!count_only) {
                    if (cb)
                        (*cb)(RA_LUA_PLUGINS, name, client_data);
                    if (lua_load_script(filename,0)) {
                        wslua_add_plugin(g_strdup(name), g_strdup(""), g_strdup(filename));
                    }
                }
                plugins_counter++;
            }
            g_free(filename);
        }
        ws_dir_close(dir);
    }

    return plugins_counter;
}

int wslua_count_plugins(void) {
    gchar* filename;
    int plugins_counter;

    /* count global scripts */
    plugins_counter = lua_load_plugins(get_plugin_dir(), NULL, NULL, TRUE);

    /* count users init.lua */
    filename = get_persconffile_path("init.lua", FALSE);
    if ((file_exists(filename))) {
        plugins_counter++;
    }
    g_free(filename);

    /* count user scripts */
    filename = get_plugins_pers_dir();
    plugins_counter += lua_load_plugins(filename, NULL, NULL, TRUE);
    g_free(filename);

    /* count scripts from command line */
    plugins_counter += ex_opt_count("lua_script");

    return plugins_counter;
}

void wslua_plugins_get_descriptions(wslua_plugin_description_callback callback, void *user_data) {
    wslua_plugin  *lua_plug;

    for (lua_plug = wslua_plugin_list; lua_plug != NULL; lua_plug = lua_plug->next)
    {
        callback(lua_plug->name, lua_plug->version, "lua script",
                 lua_plug->filename, user_data);
    }
}

static void
print_wslua_plugin_description(const char *name, const char *version,
                               const char *description, const char *filename,
                               void *user_data _U_)
{
    printf("%s\t%s\t%s\t%s\n", name, version, description, filename);
}

void
wslua_plugins_dump_all(void)
{
    wslua_plugins_get_descriptions(print_wslua_plugin_description, NULL);
}

int wslua_init(register_cb cb, gpointer client_data) {
    gchar* filename;
    const gchar *script_filename;
    const funnel_ops_t* ops = funnel_get_funnel_ops();
    gboolean run_anyway = FALSE;
    expert_module_t* expert_lua;
    int file_count = 1;

    static ei_register_info ei[] = {
        { &ei_lua_error, { "_ws.lua.error", PI_UNDECODED, PI_ERROR ,"Lua Error", EXPFILL }},
    };

    /* set up the logger */
    g_log_set_handler(LOG_DOMAIN_LUA, (GLogLevelFlags)(G_LOG_LEVEL_CRITICAL|
                      G_LOG_LEVEL_WARNING|
                      G_LOG_LEVEL_MESSAGE|
                      G_LOG_LEVEL_INFO|
                      G_LOG_LEVEL_DEBUG),
                      ops ? ops->logger : basic_logger,
                      NULL);

    if (!L) {
        L = luaL_newstate();
    }

    WSLUA_INIT(L);

    proto_lua = proto_register_protocol("Lua Dissection", "Lua Dissection", "_ws.lua");
    expert_lua = expert_register_protocol(proto_lua);
    expert_register_field_array(expert_lua, ei, array_length(ei));

    lua_atpanic(L,wslua_panic);

    /* the init_routines table (accessible by the user) */
    lua_newtable (L);
    lua_setglobal(L, WSLUA_INIT_ROUTINES);

    /* the dissectors table goes in the registry (not accessible) */
    lua_newtable (L);
    lua_dissectors_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable (L);
    lua_heur_dissectors_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* the preferences apply_cb table (accessible by the user) */
    lua_newtable (L);
    lua_setglobal(L, WSLUA_PREFS_CHANGED);

    /* set running_superuser variable to its proper value */
    WSLUA_REG_GLOBAL_BOOL(L,"running_superuser",started_with_special_privs());

    /* special constant used by PDU reassembly handling */
    /* see dissect_lua() for notes */
    WSLUA_REG_GLOBAL_NUMBER(L,"DESEGMENT_ONE_MORE_SEGMENT",DESEGMENT_ONE_MORE_SEGMENT);

    /* load system's init.lua */
    if (running_in_build_directory()) {
        /* Running from build directory, load generated file */
        filename = g_strdup_printf("%s" G_DIR_SEPARATOR_S "epan" G_DIR_SEPARATOR_S "wslua"
                                   G_DIR_SEPARATOR_S "init.lua", get_progfile_dir());
    } else {
        filename = get_datafile_path("init.lua");
    }

    if (( file_exists(filename))) {
        lua_load_script(filename,0);
    }

    g_free(filename);
    filename = NULL;

    /* check if lua is to be disabled */
    lua_getglobal(L,"disable_lua");

    if (lua_isboolean(L,-1) && lua_toboolean(L,-1)) {
        /* disable lua */
        lua_close(L);
        L = NULL;
        return 0;
    }
    lua_pop(L,1);  /* pop the getglobal result */

    /* load global scripts */
    lua_load_plugins(get_plugin_dir(), cb, client_data, FALSE);

    /* check whether we should run other scripts even if running superuser */
    lua_getglobal(L,"run_user_scripts_when_superuser");

    if (lua_isboolean(L,-1) && lua_toboolean(L,-1)) {
        run_anyway = TRUE;
    }
    lua_pop(L,1);  /* pop the getglobal result */

    /* if we are indeed superuser run user scripts only if told to do so */
    if ( (!started_with_special_privs()) || run_anyway ) {
        /* load users init.lua */
        filename = get_persconffile_path("init.lua", FALSE);
        if ((file_exists(filename))) {
            if (cb)
                (*cb)(RA_LUA_PLUGINS, get_basename(filename), client_data);
            lua_load_script(filename,0);
        }
        g_free(filename);

        /* load user scripts */
        filename = get_plugins_pers_dir();
        lua_load_plugins(filename, cb, client_data, FALSE);
        g_free(filename);

        /* load scripts from command line */
        while((script_filename = ex_opt_get_next("lua_script"))) {
            if (cb)
                (*cb)(RA_LUA_PLUGINS, get_basename(script_filename), client_data);
            lua_load_script(script_filename,file_count);
            file_count++;
        }
    }

    /* at this point we're set up so register the init routine */
    register_init_routine(wslua_init_routine);

    /*
     * after this point it is too late to register a menu
     * disable the function to avoid weirdness
     */
    lua_pushcfunction(L, wslua_not_register_menu);
    lua_setglobal(L, "register_menu");

    /* set up some essential globals */
    lua_pinfo = NULL;
    lua_tree = NULL;
    lua_tvb = NULL;

    lua_data_handle = find_dissector("data");

    Proto_commit(L);

    return 0;
}

int wslua_cleanup(void) {
    /* cleanup lua */
    lua_close(L);
    L = NULL;
    return 0;
}

lua_State* wslua_state(void) { return L; }

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=4 expandtab:
 * :indentSize=4:tabSize=4:noTabs=true:
 */

