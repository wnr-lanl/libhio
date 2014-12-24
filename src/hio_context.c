/* -*- Mode: C; c-basic-offset:2 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @file context.c
 * @brief hio context implementation
 */

#include "hio_types.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/param.h>

static hio_context_t hio_context_alloc (const char *identifier) {
  hio_context_t new_context;
  int rc;

  new_context = (hio_context_t) calloc (1, sizeof (*new_context));
  if (NULL == new_context) {
    return NULL;
  }

  new_context->context_object.identifier = strdup (identifier);
  if (NULL == new_context->context_object.identifier) {
    free (new_context);
    return NULL;
  }

  rc = hioi_config_init (&new_context->context_object);
  if (HIO_SUCCESS != rc) {
    free (new_context->context_object.identifier);
    free (new_context);

    return NULL;
  }

  /* default context configuration */
  new_context->context_verbose          = HIO_VERBOSE_ERROR;
  new_context->context_checkpoint_size  = 0;
  new_context->context_print_statistics = false;
  new_context->context_rank = 0;
  new_context->context_size = 1;
  new_context->context_last_checkpoint.tv_sec   = 0;

  new_context->context_file_configuration = NULL;
  new_context->context_file_configuration_size = 0;
  new_context->context_file_configuration_count = 0;

  pthread_mutex_init (&new_context->context_lock, NULL);

  return new_context;
}

/**
 * Release all resources associated with a context
 */
static void hio_context_release (hio_context_t *contextp) {
  hio_context_t context = *contextp;
  int rc;

  /* clean up the context mutex */
  pthread_mutex_destroy (&context->context_lock);

  for (int i = 0 ; i < context->context_module_count ; ++i) {
    context->context_modules[i]->fini (context->context_modules[i]);
  }

  if (context->context_file_configuration) {
    for (int i = 0 ; i < context->context_file_configuration_count ; ++i) {
      hio_config_kv_t *kv = context->context_file_configuration + i;
      if (kv->key) {
        free (kv->key);
      }
      if (kv->value) {
        free (kv->value);
      }
      if (kv->object_identifier) {
        free (kv->object_identifier);
      }
    }
    free (context->context_file_configuration);
  }

  /* clean up any mpi resources */
#if HIO_USE_MPI
  if (context->context_comm) {
    rc = MPI_Comm_free (&context->context_comm);
    if (MPI_SUCCESS != rc) {
      hio_err_push_mpi (HIO_ERR_OUT_OF_RESOURCE, NULL, NULL, rc, "Error freeing MPI communicator");
    }
  }
#endif

  /* finalize the configuration */
  hioi_config_fini (&context->context_object);

  if (context->context_object.identifier) {
    free (context->context_object.identifier);
  }

  if (context->context_data_roots) {
    free (context->context_data_roots);
  }

  free (context);
  *contextp = NULL;
}

static int hioi_context_create_modules (hio_context_t context) {
  char *data_roots, *data_root, *last;
  hio_module_t *module = NULL;
  int num_modules = 0;
  int rc = HIO_SUCCESS;

  data_roots = strdup (context->context_data_roots);
  if (NULL == data_roots) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  data_root = strtok_r (data_roots, ",", &last);

  do {
    rc = hioi_component_query (context, data_root, &module);
    if (HIO_SUCCESS != rc) {
      hio_err_push (rc, context, NULL, "Could not find an hio io module for data root %s",
                    data_root);
      break;
    }

    context->context_modules[num_modules++] = module;
    if (HIO_MAX_DATA_ROOTS == num_modules) {
      hioi_log (context, HIO_VERBOSE_WARN, "Maximum number of IO modules reached for this context");
      break;
    }
  } while (NULL != (data_root = strtok_r (NULL, ",", &last)));

  context->context_module_count = num_modules;
  context->context_current_module = 0;

  free (data_roots);

  return rc;
}

static int hio_init_common (hio_context_t context, const char *config_file, const char *config_file_prefix,
                            const char *context_name) {

  char cwd_buffer[MAXPATHLEN] = "";
  int rc;

  pthread_mutex_init (&context->context_lock, NULL);

  rc = hioi_component_init ();
  if (HIO_SUCCESS != rc) {
    hio_err_push (rc, NULL, NULL, "Could not initialize the hio component interface");
    return rc;
  }

  rc = hioi_config_parse (context, config_file, config_file_prefix);
  if (HIO_SUCCESS != rc) {
    return rc;
  }

  /* default data root is the current working directory */
  getcwd (cwd_buffer, MAXPATHLEN);

  rc = asprintf (&context->context_data_roots, "posix:%s", cwd_buffer);
  if (0 > rc) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  hioi_config_add (context, &context->context_object, &context->context_verbose,
                   "context_verbose", HIO_CONFIG_TYPE_UINT32, NULL, "Debug level", 0);

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Set context verbosity to %d", context->context_verbose);

  /* data roots can not be changed after a context has been created */
  hioi_config_add (context, &context->context_object, &context->context_data_roots,
                   "context_data_roots", HIO_CONFIG_TYPE_STRING, NULL,
                   "Comma-separated list of data roots to use with this context",
                   HIO_VAR_FLAG_READONLY);

  hioi_config_add (context, &context->context_object, &context->context_checkpoint_size,
                   "context_checkpoint_size", HIO_CONFIG_TYPE_UINT64, NULL,
                   "hio hint for expected checkpoint size (default: 0 -- auto)", 0);

  hioi_config_add (context, &context->context_object, &context->context_print_statistics,
                   "context_print_statistics", HIO_CONFIG_TYPE_BOOL, NULL, "Print statistics "
                   "to stdout when the context is closed (default: 0)", 0);

  if (context->context_verbose > HIO_VERBOSE_MAX) {
    context->context_verbose = HIO_VERBOSE_MAX;
  }

  /* create hio modules for each item in the specified data roots */
  return hioi_context_create_modules (context);
}

int hio_init_single (hio_context_t *new_context, const char *config_file, const char *config_file_prefix,
                     const char *context_name) {
  hio_context_t context;
  int rc;

  context = hio_context_alloc (context_name);
  if (NULL == context) {
    hio_err_push (HIO_ERR_OUT_OF_RESOURCE, NULL, NULL, "Could not allocate space for new hio context");
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  rc = hio_init_common (context, config_file, config_file_prefix, context_name);
  if (HIO_SUCCESS != rc) {
    hio_context_release (&context);
    return rc;
  }

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Created new single context with identifier %s",
            context_name);

  *new_context = context;

  return HIO_SUCCESS;
}

#if HIO_USE_MPI
int hio_init_mpi (hio_context_t *new_context, MPI_Comm *comm, const char *config_file, const char *config_file_prefix,
		  const char *context_name) {
  hio_context_t context;
  MPI_Comm comm_in;
  int rc, flag = 0;

  (void) MPI_Initialized (&flag);
  if (!flag) {
    hio_err_push (HIO_ERROR, NULL, NULL, "Attempted to initialize hio before MPI");
    return HIO_ERROR;
  }

  (void) MPI_Finalized (&flag);
  if (flag) {
    hio_err_push (HIO_ERROR, NULL, NULL, "Attempted to initialize hio after MPI was finalized");
    return HIO_ERROR;
  }

  context = hio_context_alloc (context_name);
  if (NULL == context) {
    hio_err_push (HIO_ERR_OUT_OF_RESOURCE, NULL, NULL, "Could not allocate space for new hio context");
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  comm_in = comm ? *comm : MPI_COMM_WORLD;

  rc = MPI_Comm_dup (comm_in, &contex->context_comm);
  if (NULL == context->context_comm) {
    hio_err_push_mpi (HIO_ERR_OUT_OF_RESOURCE, context, NULL, rc,
                      "Error duplicating MPI communicator");
    hio_context_release(&context);
    return hio_err_mpi (rc);
  }

  MPI_Comm_rank (context->context_comm, &context->context_rank);
  MPI_Comm_size (context->context_comm, &context->context_size);

  rc = hio_init_common (context, config_file, config_file_prefix, context_name);
  if (HIO_SUCCESS != rc) {
    hio_context_release(&context);
    return rc;
  }

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Created new mpi context with identifier %s",
            context_name);

  *new_context = context;

  return HIO_SUCCESS;
}
#endif

int hio_fini (hio_context_t *context) {
  if (NULL == context || NULL == *context) {
    return HIO_SUCCESS;
  }

  hioi_log (*context, HIO_VERBOSE_DEBUG_LOW, "Destroying context with identifier %s",
            (*context)->context_object.identifier);

  hio_context_release (context);
  return hioi_component_fini ();
}

void hio_should_checkpoint (hio_context_t ctx, int *hint) {
  /* TODO -- implement this */
  *hint = HIO_SCP_MUST_CHECKPOINT;
}

hio_module_t *hioi_context_select_module (hio_context_t context) {
  /* TODO -- finish implementation */
  if (-1 == context->context_current_module) {
    context->context_current_module = 0;
  }

  return context->context_modules[context->context_current_module];
}