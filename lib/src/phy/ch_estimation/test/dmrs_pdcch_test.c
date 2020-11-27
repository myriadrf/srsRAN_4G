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

#include "srslte/common/test_common.h"
#include "srslte/phy/ch_estimation/dmrs_pdcch.h"
#include "srslte/phy/phch/pdcch_nr.h"
#include <complex.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

static srslte_carrier_nr_t    carrier  = {};
static srslte_dmrs_pdcch_ce_t pdcch_ce = {};
static uint16_t               rnti     = 0x1234;

void usage(char* prog)
{
  printf("Usage: %s [recov]\n", prog);

  printf("\t-r nof_prb [Default %d]\n", carrier.nof_prb);
  printf("\t-e extended cyclic prefix [Default normal]\n");

  printf("\t-c cell_id [Default %d]\n", carrier.id);

  printf("\t-v increase verbosity\n");
}

static void parse_args(int argc, char** argv)
{
  int opt;
  while ((opt = getopt(argc, argv, "recov")) != -1) {
    switch (opt) {
      case 'r':
        carrier.nof_prb = (uint32_t)strtol(argv[optind], NULL, 10);
        break;
      case 'c':
        carrier.id = (uint32_t)strtol(argv[optind], NULL, 10);
        break;
      case 'v':
        srslte_verbose++;
        break;
      default:
        usage(argv[0]);
        exit(-1);
    }
  }
}

static int run_test(srslte_dmrs_pdcch_estimator_t* estimator,
                    const srslte_coreset_t*        coreset,
                    const srslte_search_space_t*   search_space,
                    uint32_t                       aggregation_level,
                    cf_t*                          sf_symbols,
                    srslte_dmrs_pdcch_ce_t*        ce)
{
  srslte_dl_slot_cfg_t slot_cfg = {};

  srslte_dci_location_t dci_location = {};
  dci_location.L                     = aggregation_level;

  for (slot_cfg.idx = 0; slot_cfg.idx < SRSLTE_NSLOTS_PER_FRAME_NR(carrier.numerology); slot_cfg.idx++) {
    uint32_t locations[SRSLTE_SEARCH_SPACE_MAX_NOF_CANDIDATES_NR] = {};

    int nof_locations =
        srslte_pdcch_nr_locations_coreset(coreset, search_space, rnti, aggregation_level, slot_cfg.idx, locations);

    TESTASSERT(nof_locations == search_space->nof_candidates[aggregation_level]);

    for (uint32_t candidate = 0; candidate < nof_locations; candidate++) {
      dci_location.ncce = locations[candidate];

      uint32_t nof_re = carrier.nof_prb * SRSLTE_NRE * SRSLTE_NSYMB_PER_SLOT_NR;
      srslte_vec_cf_zero(sf_symbols, nof_re);

      TESTASSERT(srslte_dmrs_pdcch_put(&carrier, coreset, &slot_cfg, &dci_location, sf_symbols) == SRSLTE_SUCCESS);

      TESTASSERT(srslte_dmrs_pdcch_estimate(estimator, &slot_cfg, sf_symbols) == SRSLTE_SUCCESS);

      srslte_dmrs_pdcch_measure_t measure = {};
      TESTASSERT(srslte_dmrs_pdcch_get_measure(estimator, &dci_location, &measure) == SRSLTE_SUCCESS);

      if (fabsf(measure.rsrp - 1.0f) > 1e-2) {
        printf("EPRE=%f; RSRP=%f; CFO=%f; SYNC_ERR=%f;\n",
               measure.epre,
               measure.rsrp,
               measure.cfo_hz,
               measure.sync_error_us);
      }
      TESTASSERT(fabsf(measure.epre - 1.0f) < 1e-3f);
      TESTASSERT(fabsf(measure.rsrp - 1.0f) < 1e-3f);
      TESTASSERT(fabsf(measure.cfo_hz) < 1e-3f);
      TESTASSERT(fabsf(measure.sync_error_us) < 1e-3f);

      TESTASSERT(srslte_dmrs_pdcch_get_ce(estimator, &dci_location, ce) == SRSLTE_SUCCESS);
      float avg_pow     = srslte_vec_avg_power_cf(ce->ce, ce->nof_re);
      float avg_pow_err = fabsf(avg_pow - 1.0f);
      TESTASSERT(ce->nof_re == ((SRSLTE_NRE - 3) * (1U << aggregation_level) * 6U));
      TESTASSERT(avg_pow_err < 0.1f);
    }
  }

  return SRSLTE_SUCCESS;
}

int main(int argc, char** argv)
{
  int ret = SRSLTE_ERROR;

  carrier.nof_prb = 50;

  parse_args(argc, argv);

  srslte_coreset_t              coreset      = {};
  srslte_search_space_t         search_space = {};
  srslte_dmrs_pdcch_estimator_t estimator    = {};

  uint32_t nof_re     = carrier.nof_prb * SRSLTE_NRE * SRSLTE_NSYMB_PER_SLOT_NR;
  cf_t*    sf_symbols = srslte_vec_cf_malloc(nof_re);

  uint32_t test_counter = 0;
  uint32_t test_passed  = 0;

  coreset.mapping_type = srslte_coreset_mapping_type_non_interleaved;

  uint32_t nof_frequency_resource = SRSLTE_MIN(SRSLTE_CORESET_FREQ_DOMAIN_RES_SIZE, carrier.nof_prb / 6);
  for (uint32_t frequency_resources = 1; frequency_resources < (1U << nof_frequency_resource); frequency_resources++) {
    uint32_t nof_freq_resources = 0;
    for (uint32_t i = 0; i < nof_frequency_resource; i++) {
      uint32_t mask             = ((frequency_resources >> i) & 1U);
      coreset.freq_resources[i] = (mask == 1);
      nof_freq_resources += mask;
    }

    for (coreset.duration = 1; coreset.duration <= 3; coreset.duration++) {

      for (search_space.type = srslte_search_space_type_common; search_space.type <= srslte_search_space_type_ue;
           search_space.type++) {

        for (uint32_t i = 0; i < SRSLTE_SEARCH_SPACE_NOF_AGGREGATION_LEVELS_NR; i++) {
          uint32_t L                     = 1U << i;
          uint32_t nof_reg               = coreset.duration * nof_freq_resources * 6;
          uint32_t nof_cce               = nof_reg / 6;
          search_space.nof_candidates[i] = SRSLTE_MIN(nof_cce / L, SRSLTE_SEARCH_SPACE_MAX_NOF_CANDIDATES_NR);
        }

        for (uint32_t aggregation_level = 0; aggregation_level < SRSLTE_SEARCH_SPACE_NOF_AGGREGATION_LEVELS_NR;
             aggregation_level++) {

          srslte_dmrs_pdcch_estimator_init(&estimator, &carrier, &coreset);

          if (run_test(&estimator, &coreset, &search_space, aggregation_level, sf_symbols, &pdcch_ce)) {
            ERROR("Test %d failed\n", test_counter);
          } else {
            test_passed++;
          }
          test_counter++;
        }
      }
    }
  }

  srslte_dmrs_pdcch_estimator_free(&estimator);

  if (sf_symbols) {
    free(sf_symbols);
  }

  ret = test_passed == test_counter ? SRSLTE_SUCCESS : SRSLTE_ERROR;
  printf("%s, %d of %d test passed successfully.\n", ret ? "Failed" : "Passed", test_passed, test_counter);

  return ret;
}
