/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srslte/phy/phch/pdcch_nr.h"
#include "srslte/phy/utils/debug.h"
#include "srslte/phy/utils/vector.h"

/**
 * @brief Recursive Y_p_n function
 */
static uint32_t srslte_pdcch_calculate_Y_p_n(uint32_t coreset_id, uint16_t rnti, int n)
{
  static const uint32_t A_p[3] = {39827, 39829, 39839};
  const uint32_t        D      = 65537;

  if (n < 0) {
    return rnti;
  }

  return (A_p[coreset_id % 3] * srslte_pdcch_calculate_Y_p_n(coreset_id, rnti, n - 1)) % D;
}

/**
 * Calculates the Control Channnel Element As described in 3GPP 38.213 R15 10.1 UE procedure for determining physical
 * downlink control channel assignment
 *
 */
static int srslte_pdcch_nr_get_ncce(const srslte_coreset_t*      coreset,
                                    const srslte_search_space_t* search_space,
                                    uint16_t                     rnti,
                                    uint32_t                     aggregation_level,
                                    uint32_t                     slot_idx,
                                    uint32_t                     candidate)
{
  if (aggregation_level >= SRSLTE_SEARCH_SPACE_NOF_AGGREGATION_LEVELS_NR) {
    ERROR("Invalid aggregation level %d;\n", aggregation_level);
    return SRSLTE_ERROR;
  }

  uint32_t L    = 1U << aggregation_level;                         // Aggregation level
  uint32_t n_ci = 0;                                               //  Carrier indicator field
  uint32_t m    = candidate;                                       // Selected PDDCH candidate
  uint32_t M    = search_space->nof_candidates[aggregation_level]; // Number of aggregation levels

  if (M == 0) {
    ERROR("Invalid number of candidates %d for aggregation level %d\n", M, aggregation_level);
    return SRSLTE_ERROR;
  }

  // Every REG is 1PRB wide and a CCE is 6 REG. So, the number of N_CCE is a sixth of the bandwidth times the number of
  // symbols
  uint32_t N_cce = srslte_coreset_get_bw(coreset) * coreset->duration / 6;

  if (N_cce < L) {
    ERROR("Error number of CCE %d is lower than the aggregation level %d\n", N_cce, L);
    return SRSLTE_ERROR;
  }

  // Calculate Y_p_n for UE search space only
  uint32_t Y_p_n = 0;
  if (search_space->type == srslte_search_space_type_ue) {
    Y_p_n = srslte_pdcch_calculate_Y_p_n(coreset->id, rnti, slot_idx);
  }

  return (int)(L * ((Y_p_n + (m * N_cce) / (L * M) + n_ci) % (N_cce / L)));
}

int srslte_pdcch_nr_locations_coreset(const srslte_coreset_t*      coreset,
                                      const srslte_search_space_t* search_space,
                                      uint16_t                     rnti,
                                      uint32_t                     aggregation_level,
                                      uint32_t                     slot_idx,
                                      uint32_t                     locations[SRSLTE_SEARCH_SPACE_MAX_NOF_CANDIDATES_NR])
{
  if (coreset == NULL || search_space == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  uint32_t nof_candidates = search_space->nof_candidates[aggregation_level];

  nof_candidates = SRSLTE_MIN(nof_candidates, SRSLTE_SEARCH_SPACE_MAX_NOF_CANDIDATES_NR);

  for (uint32_t candidate = 0; candidate < nof_candidates; candidate++) {
    int ret = srslte_pdcch_nr_get_ncce(coreset, search_space, rnti, aggregation_level, slot_idx, candidate);
    if (ret < SRSLTE_SUCCESS) {
      return ret;
    }

    locations[candidate] = ret;
  }

  return nof_candidates;
}

static int pdcch_nr_init_common(srslte_pdcch_nr_t* q, const srslte_pdcch_nr_args_t* args)
{
  if (q == NULL || args == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  q->c = srslte_vec_u8_malloc(SRSLTE_PDCCH_MAX_RE * 2);
  if (q->c == NULL) {
    return SRSLTE_ERROR;
  }

  q->d = srslte_vec_u8_malloc(SRSLTE_PDCCH_MAX_RE * 2);
  if (q->d == NULL) {
    return SRSLTE_ERROR;
  }

  q->f = srslte_vec_u8_malloc(SRSLTE_PDCCH_MAX_RE * 2);
  if (q->f == NULL) {
    return SRSLTE_ERROR;
  }

  q->symbols = srslte_vec_cf_malloc(SRSLTE_PDCCH_MAX_RE);
  if (q->symbols == NULL) {
    return SRSLTE_ERROR;
  }

  if (srslte_crc_init(&q->crc24c, SRSLTE_LTE_CRC24C, 24) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  if (srslte_polar_code_init(&q->code) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  srslte_modem_table_lte(&q->modem_table, SRSLTE_MOD_QPSK);

  return SRSLTE_SUCCESS;
}

int srslte_pdcch_nr_init_tx(srslte_pdcch_nr_t* q, const srslte_pdcch_nr_args_t* args)
{
  if (pdcch_nr_init_common(q, args) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }
  q->is_tx = true;

  srslte_polar_encoder_type_t encoder_type =
      (args->disable_simd) ? SRSLTE_POLAR_ENCODER_PIPELINED : SRSLTE_POLAR_ENCODER_AVX2;

  if (srslte_polar_encoder_init(&q->encoder, encoder_type, NMAX_LOG) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  if (srslte_polar_rm_tx_init(&q->rm) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}

int srslte_pdcch_nr_init_rx(srslte_pdcch_nr_t* q, const srslte_pdcch_nr_args_t* args)
{
  if (pdcch_nr_init_common(q, args) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  srslte_polar_decoder_type_t decoder_type =
      (args->disable_simd) ? SRSLTE_POLAR_DECODER_SSC_C : SRSLTE_POLAR_DECODER_SSC_C_AVX2;

  if (srslte_polar_decoder_init(&q->decoder, decoder_type, NMAX_LOG) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  if (srslte_polar_rm_rx_init_c(&q->rm) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  if (args->measure_evm) {
    q->evm_buffer = srslte_evm_buffer_alloc(SRSLTE_PDCCH_MAX_RE * 2);
  }

  return SRSLTE_SUCCESS;
}

void srslte_pdcch_nr_init_free(srslte_pdcch_nr_t* q)
{
  if (q == NULL) {
    return;
  }

  srslte_polar_code_free(&q->code);

  if (q->is_tx) {
    srslte_polar_encoder_free(&q->encoder);
    srslte_polar_rm_tx_free(&q->rm);
  } else {
    srslte_polar_decoder_free(&q->decoder);
    srslte_polar_rm_rx_free_c(&q->rm);
  }

  if (q->c) {
    free(q->c);
  }

  if (q->d) {
    free(q->d);
  }

  if (q->f) {
    free(q->f);
  }

  srslte_modem_table_free(&q->modem_table);

  SRSLTE_MEM_ZERO(q, srslte_pdcch_nr_t, 1);
}

int srslte_pdcch_nr_set_carrier(srslte_pdcch_nr_t*         q,
                                const srslte_carrier_nr_t* carrier,
                                const srslte_coreset_t*    coreset)
{
  if (q == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  if (carrier != NULL) {
    q->carrier = *carrier;
  }

  if (coreset != NULL) {
    q->coreset = *coreset;
  }

  return SRSLTE_SUCCESS;
}

static uint32_t pdcch_nr_cp(const srslte_pdcch_nr_t*     q,
                            const srslte_dci_location_t* dci_location,
                            cf_t*                        slot_grid,
                            cf_t*                        symbols,
                            bool                         put)
{
  uint32_t L = 1U << dci_location->L;

  // Calculate begin and end sub-carrier index for the selected candidate
  uint32_t k_begin = (dci_location->ncce * SRSLTE_NRE * 6) / q->coreset.duration;
  uint32_t k_end   = k_begin + (L * 6 * SRSLTE_NRE) / q->coreset.duration;

  uint32_t count = 0;

  // Iterate over symbols
  for (uint32_t l = 0; l < q->coreset.duration; l++) {
    // Iterate over frequency resource groups
    uint32_t k = 0;
    for (uint32_t r = 0; r < SRSLTE_CORESET_FREQ_DOMAIN_RES_SIZE; r++) {
      if (q->coreset.freq_resources[r]) {
        for (uint32_t i = r * 6 * SRSLTE_NRE; i < (r + 1) * 6 * SRSLTE_NRE; i++, k++) {
          if (k >= k_begin && k < k_end && k % 4 != 1) {
            if (put) {
              slot_grid[q->carrier.nof_prb * SRSLTE_NRE * l + i] = symbols[count++];
            } else {
              symbols[count++] = slot_grid[q->carrier.nof_prb * SRSLTE_NRE * l + i];
            }
          }
        }
      }
    }
  }

  return count;
}

uint32_t pdcch_nr_c_init(const srslte_pdcch_nr_t* q, const srslte_dci_msg_nr_t* dci_msg)
{
  uint32_t n_id = (dci_msg->search_space == srslte_search_space_type_ue && q->coreset.dmrs_scrambling_id_present)
                      ? q->coreset.dmrs_scrambling_id
                      : q->carrier.id;
  uint32_t n_rnti = (dci_msg->search_space == srslte_search_space_type_ue && q->coreset.dmrs_scrambling_id_present)
                        ? dci_msg->rnti
                        : 0U;
  return ((n_rnti << 16U) + n_id) & 0x7fffffffU;
}

int srslte_pdcch_nr_encode(srslte_pdcch_nr_t* q, const srslte_dci_msg_nr_t* dci_msg, cf_t* slot_symbols)
{

  if (q == NULL || dci_msg == NULL || slot_symbols == NULL) {
    return SRSLTE_ERROR;
  }

  // Calculate...
  uint32_t K = dci_msg->nof_bits + 24U;                              // Payload size including CRC
  uint32_t M = (1U << dci_msg->location.L) * (SRSLTE_NRE - 3U) * 6U; // Number of RE
  uint32_t E = M * 2;                                                // Number of Rate-Matched bits

  // Get polar code
  if (srslte_polar_code_get(&q->code, K, E, 9U) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Copy DCI message
  srslte_vec_u8_copy(q->c, dci_msg->payload, dci_msg->nof_bits);

  // Append CRC
  srslte_crc_attach(&q->crc24c, q->c, dci_msg->nof_bits);

  // Unpack RNTI
  uint8_t  unpacked_rnti[16] = {};
  uint8_t* ptr               = unpacked_rnti;
  srslte_bit_unpack(dci_msg->rnti, &ptr, 16);

  // Scramble CRC with RNTI
  srslte_vec_xor_bbb(unpacked_rnti, &q->c[K - 16], &q->c[K - 16], 16);

  // Encode bits
  if (srslte_polar_encoder_encode(&q->encoder, q->c, q->d, q->code.n) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Rate matching
  srslte_polar_rm_tx(&q->rm, q->d, q->f, q->code.n, E, K, 0);

  // Scrambling
  srslte_sequence_apply_bit(q->f, q->f, E, pdcch_nr_c_init(q, dci_msg));

  // Modulation
  srslte_mod_modulate(&q->modem_table, q->f, q->symbols, E);

  // Put symbols in grid
  uint32_t m = pdcch_nr_cp(q, &dci_msg->location, slot_symbols, q->symbols, true);
  if (M != m) {
    ERROR("Unmatch number of RE (%d != %d)\n", m, M);
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}

int srslte_pdcch_nr_decode(srslte_pdcch_nr_t*      q,
                           cf_t*                   slot_symbols,
                           srslte_dmrs_pdcch_ce_t* ce,
                           srslte_dci_msg_nr_t*    dci_msg,
                           srslte_pdcch_nr_res_t*  res)
{
  if (q == NULL || dci_msg == NULL || ce == NULL || slot_symbols == NULL || res == NULL) {
    return SRSLTE_ERROR;
  }

  // Calculate...
  uint32_t K = dci_msg->nof_bits + 24U;                              // Payload size including CRC
  uint32_t M = (1U << dci_msg->location.L) * (SRSLTE_NRE - 3U) * 6U; // Number of RE
  uint32_t E = M * 2;                                                // Number of Rate-Matched bits

  // Check number of estimates is correct
  if (ce->nof_re != M) {
    ERROR("Invalid number of channel estimates (%d != %d)\n", M, ce->nof_re);
    return SRSLTE_ERROR;
  }

  // Get polar code
  if (srslte_polar_code_get(&q->code, K, E, 9U) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Get symbols from grid
  uint32_t m = pdcch_nr_cp(q, &dci_msg->location, slot_symbols, q->symbols, false);
  if (M != m) {
    ERROR("Unmatch number of RE (%d != %d)\n", m, M);
  }

  // Equalise
  srslte_predecoding_single(q->symbols, ce->ce, q->symbols, NULL, M, 1.0f, ce->noise_var);

  // Demodulation
  int8_t* llr = (int8_t*)q->f;
  srslte_demod_soft_demodulate_b(SRSLTE_MOD_QPSK, q->symbols, llr, M);

  // Measure EVM if configured
  if (q->evm_buffer != NULL) {
    res->evm = srslte_evm_run_b(q->evm_buffer, &q->modem_table, q->symbols, llr, E);
  } else {
    res->evm = NAN;
  }

  // Descrambling
  srslte_sequence_apply_c(llr, llr, E, pdcch_nr_c_init(q, dci_msg));

  // Un-rate matching
  int8_t* d = (int8_t*)q->d;
  if (srslte_polar_rm_rx_c(&q->rm, llr, d, E, q->code.n, K, 0) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Decode
  if (srslte_polar_decoder_decode_c(&q->decoder, d, q->c, q->code.n, q->code.F_set, q->code.F_set_size) <
      SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Unpack RNTI
  uint8_t  unpacked_rnti[16] = {};
  uint8_t* ptr               = unpacked_rnti;
  srslte_bit_unpack(dci_msg->rnti, &ptr, 16);

  // De-Scramble CRC with RNTI
  ptr = &q->c[K - 24];
  srslte_vec_xor_bbb(unpacked_rnti, &q->c[K - 16], &q->c[K - 16], 16);

  // Check CRC
  uint32_t checksum1 = srslte_crc_checksum(&q->crc24c, q->c, dci_msg->nof_bits);
  uint32_t checksum2 = srslte_bit_pack(&ptr, 24);
  res->crc           = checksum1 == checksum2;

  // Copy DCI message
  srslte_vec_u8_copy(dci_msg->payload, q->c, dci_msg->nof_bits);

  return SRSLTE_SUCCESS;
}