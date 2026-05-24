/**
 * @file persistent_buffer.h
 * @brief Compile-time backend abstraction for persistent storage tier.
 * @ingroup buffer_mgr
 *
 * @details
 * Provides a uniform API (`persistent_buffer_*`) that maps to the selected
 * persistent storage backend at compile time.
 *
 * Usage:
 *   make build                       → Em_EEPROM (default, internal flash)
 *   make build BUFFER_BACKEND=QSPI   → QSPI NOR flash (S25FL512S)
 *
 * @see flash_buffer.h, qspi_buffer.h
 */

#pragma once

#if defined(BUFFER_BACKEND_QSPI)

  #include "qspi_buffer.h"

  #define persistent_buffer_init         qspi_buffer_init
  #define persistent_buffer_put_record   qspi_buffer_put_record
  #define persistent_buffer_peek         qspi_buffer_peek
  #define persistent_buffer_consume      qspi_buffer_consume
  #define persistent_buffer_depth        qspi_buffer_depth
  #define persistent_buffer_total_writes qspi_buffer_total_writes
  #define persistent_buffer_erase_all    qspi_buffer_erase_all
  #define persistent_buffer_lock         qspi_buffer_lock
  #define persistent_buffer_unlock       qspi_buffer_unlock
  #define PERSISTENT_BACKEND_NAME        "QSPI (S25FL512S)"
  #define PERSISTENT_BACKEND_ID          (1u)

#else

  #include "flash_buffer.h"

  #define persistent_buffer_init         flash_buffer_init
  #define persistent_buffer_put_record   flash_buffer_put_record
  #define persistent_buffer_peek         flash_buffer_peek
  #define persistent_buffer_consume      flash_buffer_consume
  #define persistent_buffer_depth        flash_buffer_depth
  #define persistent_buffer_total_writes flash_buffer_total_writes
  #define persistent_buffer_erase_all    flash_buffer_erase_all
  #define persistent_buffer_lock         flash_buffer_lock
  #define persistent_buffer_unlock       flash_buffer_unlock
  #define PERSISTENT_BACKEND_NAME        "Em_EEPROM (internal)"
  #define PERSISTENT_BACKEND_ID          (0u)

#endif

/** Sentinel written to storage_backend gauge when the persistent tier
 *  failed to initialise or is disabled — encodes as "none" in metrics JSON. */
#define PERSISTENT_BACKEND_ID_NONE     (0xFFu)
