#include <flux/core.h>
#include <flux/jobtap.h>
#include <jansson.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* Configuration structure */
typedef struct {
    char **uris;        /* Array of cluster URIs */
    int count;          /* Number of clusters */
    int initialized;    /* Random seed initialized flag */
} cluster_config_t;

static cluster_config_t config = {0};

extern int flux_jobtap_call(flux_plugin_t *p,
                            flux_jobid_t      id,
                            const char       *topic,
                            flux_plugin_arg_t *args);

/* Load cluster URIs from config file */
static int load_config(flux_plugin_t *p, const char *path)
{
    flux_t *h = flux_jobtap_get_flux(p);
    json_t *root;
    json_error_t error;
    
    /* Load JSON config file */
    root = json_load_file(path, 0, &error);
    if (!root) {
        flux_log(h, LOG_ERR, "Failed to load config %s: %s", 
                 path, error.text);
        return -1;
    }
    
    /* Get clusters array */
    json_t *clusters = json_object_get(root, "clusters");
    if (!clusters || !json_is_array(clusters)) {
        flux_log(h, LOG_ERR, "Config missing 'clusters' array");
        json_decref(root);
        return -1;
    }
    
    /* Count and allocate */
    config.count = json_array_size(clusters);
    if (config.count == 0) {
        flux_log(h, LOG_ERR, "No clusters defined in config");
        json_decref(root);
        return -1;
    }
    
    config.uris = calloc(config.count, sizeof(char *));
    if (!config.uris) {
        json_decref(root);
        return -1;
    }
    
    /* Load each URI */
    for (int i = 0; i < config.count; i++) {
        json_t *cluster = json_array_get(clusters, i);
        const char *uri = NULL;
        
        if (json_is_string(cluster)) {
            /* Simple format: just URI strings */
            uri = json_string_value(cluster);
        } else if (json_is_object(cluster)) {
            /* Object format: {"uri": "...", "name": "..."} etc. */
            uri = json_string_value(json_object_get(cluster, "uri"));
        }
        
        if (!uri) {
            flux_log(h, LOG_ERR, "Invalid cluster entry at index %d", i);
            json_decref(root);
            return -1;
        }
        
        config.uris[i] = strdup(uri);
        flux_log(h, LOG_INFO, "Loaded cluster %d: %s", i, uri);
    }
    
    json_decref(root);
    
    /* Initialize random number generator */
    if (!config.initialized) {
        srand(time(NULL));
        config.initialized = 1;
    }
    
    return 0;
}

/* Select a random cluster URI */
static const char *select_random_cluster(flux_plugin_t *p)
{
    flux_t *h = flux_jobtap_get_flux(p);
    
    if (config.count == 0) {
        flux_log(h, LOG_ERR, "No clusters available");
        return NULL;
    }
    
    int index = rand() % config.count;
    flux_log(h, LOG_DEBUG, "Selected cluster %d: %s", 
             index, config.uris[index]);
    
    return config.uris[index];
}

/* Callback for job.state.new: calls delegate.submit on the chosen URI */
static int job_new_cb (flux_plugin_t *p,
                      const char *topic,
                      flux_plugin_arg_t *args,
                      void *arg)
{
    json_int_t        id; 
    json_t            *jobspec;
     const char        *selected_uri;
     flux_plugin_arg_t *delegate_args;
     int                rc;
    
    flux_t        *h      = flux_jobtap_get_flux(p);

      if (!h) {
        flux_log_error(h, "Failed initial check.");
        return flux_jobtap_reject_job (p,
                                       args,
                                       "error processing delegate: %s",
                                       flux_plugin_arg_strerror (args));
    }

    flux_log(h, LOG_INFO, "ENTERED JOB_NEW_CALLBACK.");

    if (flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN,
                               "{s:I s:o}",
                               "id",      &id,
                               "jobspec", &jobspec) < 0) {
            flux_log(h, LOG_ERR, "Error with unpacking and setting flux jobID");
            return -1;
    }

    selected_uri = select_random_cluster(p);
    // flux_log (h, LOG_INFO, "selected id is %" JSON_INTEGER_FORMAT, id);
    // flux_log(h, LOG_INFO, "jobspec is %s", json_dumps((json_t*) jobspec, JSON_INDENT(4)));
    
    if (!selected_uri) {
        flux_log(h, LOG_ERR, "No URI was selected.");
        return -1;
    }

    // flux_log(h, LOG_INFO, "SELECT PLUGIN: Delegating job  %" JSON_INTEGER_FORMAT "to %s",  
    //         id, selected_uri);

    delegate_args = flux_plugin_arg_create();
    
    if (!delegate_args) {
       flux_log(h, LOG_ERR, "No URI was selected.");
        return -1;
    }

    if (flux_plugin_arg_pack(delegate_args, FLUX_PLUGIN_ARG_OUT,
                             "{s:I s:s s:o}",
                             "id",      id,
                             "uri",     selected_uri,
                             "jobspec", jobspec) < 0) {
            flux_log(h, LOG_ERR, "SELECT PLUGIN: Could not pack items");                        
            flux_plugin_arg_destroy(delegate_args);
        return -1;
    }

    char** json_str = malloc(sizeof(json_t));
    flux_plugin_arg_get(delegate_args, FLUX_PLUGIN_ARG_OUT, json_str);
    flux_log(h, LOG_INFO, "Trying to get args %s", *json_str);

    flux_log(h, LOG_INFO, "Calling delegate.submit now. ");
    rc = flux_jobtap_call(p, FLUX_JOBTAP_CURRENT_JOB, "delegate.submit", delegate_args);
 // rc = 0;
     // rc = flux_jobtap_call(p, (flux_jobid_t)id, "delegate.submit", delegate_args);
    if (rc < 0 ) {
        flux_log(h, LOG_ERR, "JOBTAP_CALL_FAILED.");
    }
    
    //Clean up
    flux_plugin_arg_destroy(delegate_args);
    return rc;
}

static const struct flux_plugin_handler tab[] = {
    {"job.new", job_new_cb, NULL},
    {0},
};

/* Plugin initialization */
int flux_plugin_init(flux_plugin_t *p)
{
    flux_t *h = flux_jobtap_get_flux(p);
    flux_log(h, LOG_ERR, "ENTERED INIT. NEW START.");

    if (flux_plugin_register (p, "select_cluster_and_delegate", tab) < 0) {
        flux_log(h, LOG_ERR, "Failed to register select_cluster_and_delegate plugin");
        return -1;
    }

    const char *config_path;
    
    // /* Set plugin name */
    // if (flux_plugin_set_name(p, "select_cluster_and_delegate") < 0)
    //     return -1;
    
    /* Get config file path */
    // config_path = flux_plugin_get_conf(p, "config");
    if (flux_plugin_conf_unpack (p, "{s:s}", 
                                    "config", &config_path) < 0){
    // if (!config_path) {
        flux_log(h, LOG_ERR, "No config file specified. "
                 "Use: flux jobtap load plugin.so config=/path/to/config.json");
        return -1;
    }
    
    /* Load configuration */
    if (load_config(p, config_path) < 0) {
        flux_log(h, LOG_ERR, "Failed to load configuration");
        return -1;
    }
    
    flux_log(h, LOG_INFO, "Random cluster selector loaded with %d clusters", 
             config.count);
    
    return 0;
    }

/* Plugin cleanup */
void flux_plugin_fini(flux_plugin_t *p)
{
    /* Free allocated memory */
    if (config.uris) {
        for (int i = 0; i < config.count; i++) {
            free(config.uris[i]);
        }
        free(config.uris);
        config.uris = NULL;
    }
    config.count = 0;
}