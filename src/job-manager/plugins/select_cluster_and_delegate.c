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

/* Job validate callback - intercept and redirect */
static int job_validate_cb(flux_plugin_t *p,
                          const char *topic,
                          flux_plugin_arg_t *args,
                          void *arg)
{
    flux_t *h = flux_jobtap_get_flux(p);
    flux_jobid_t id;
    int urgency;
    int userid;
    
    /* Get job info */
    if (flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN,
                              "{s:I s:i s:i}",
                              "id", &id,
                              "urgency", &urgency,
                              "userid", &userid) < 0) {
        flux_log(h, LOG_ERR, "Failed to unpack job info");
        return -1;
    }
    
    /* Check if job already has delegate.uri set (avoid loops) */
    const char *existing_uri = NULL;
    flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN,
                          "{s:{s:{s:s}}}",
                          "jobspec", "attributes", "system",
                          "delegate.uri", &existing_uri);
    
    if (existing_uri) {
        /* Job already delegated, let it pass through */
        flux_log(h, LOG_DEBUG, "Job %ju already delegated to %s", 
                 id, existing_uri);
        return 0;
    }
    
    /* Select random cluster */
    const char *selected_uri = select_random_cluster(p);
    if (!selected_uri) {
        return flux_jobtap_reject_job(p, args, 
                                     "No clusters available for delegation");
    }
    
    flux_log(h, LOG_INFO, "Delegating job %ju to %s", id, selected_uri);
    
    // Add delegate dependency
    char dependency[1024];
    snprintf(dependency, sizeof(dependency), 
             "delegate:%s", selected_uri);
    
    return flux_jobtap_dependency_add(p, id, dependency);
    

    // /* Set delegate.uri attribute for the delegate plugin to use */
    // if (flux_plugin_arg_pack(args, FLUX_PLUGIN_ARG_OUT,
    //                         "{s:{s:{s:{s:s}}}}",
    //                         "jobspec", "attributes", "system",
    //                         "delegate.uri", selected_uri) < 0) {
    //     flux_log(h, LOG_ERR, "Failed to set delegate.uri");
    //     return -1;
    // }
    
    // /* Also set delegate.interactive if this is an interactive job */
    // int interactive = 0;
    // flux_plugin_arg_unpack(args, FLUX_PLUGIN_ARG_IN,
    //                       "{s:b}",
    //                       "interactive", &interactive);
    
    // if (interactive) {
    //     flux_plugin_arg_pack(args, FLUX_PLUGIN_ARG_OUT,
    //                         "{s:{s:{s:{s:b}}}}",
    //                         "jobspec", "attributes", "system",
    //                         "delegate.interactive", interactive);
    // }
    
 //   return 0;
}

/* Plugin initialization */
int flux_plugin_init(flux_plugin_t *p)
{
    flux_t *h = flux_jobtap_get_flux(p);
    const char *config_path;
    
    /* Set plugin name */
    if (flux_plugin_set_name(p, "select_cluster_and_delegate") < 0)
        return -1;
    
    /* Get config file path */
   //  config_path = flux_plugin_get_conf(p, "config");
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
    
    /* Ensure delegate plugin is loaded */
    flux_log(h, LOG_INFO, 
             "NOTE: Ensure 'delegate' plugin is loaded for delegation to work");
    
    /* Register job validate callback */
    if (flux_plugin_add_handler(p, "job.validate", 
                               job_validate_cb, NULL) < 0) {
        flux_log(h, LOG_ERR, "Failed to register job.validate callback");
        return -1;
    }
    
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