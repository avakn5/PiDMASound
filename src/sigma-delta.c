static int sd_integrator = 0;
static int sd_feedback   = 0;

void sigma_delta_reset(void)
{
    sd_integrator = 0;
    sd_feedback   = 0;
}

void sigma_delta_modulate_n(const short *pcm_in, unsigned int n_samples,
                           unsigned char *pdm_out, unsigned int oversample)
{
    unsigned int bit_index = 0;

    for (unsigned int s = 0; s < n_samples; s++) {
        int x = pcm_in[s];

        for (unsigned int i = 0; i < oversample; i++) {
            sd_integrator += x - sd_feedback;

            if (sd_integrator >= 0) {
                sd_feedback = 32767;
                pdm_out[bit_index >> 3] |=  (0x80 >> (bit_index & 7));
            } else {
                sd_feedback = -32768;
                pdm_out[bit_index >> 3] &= ~(0x80 >> (bit_index & 7));
            }
            bit_index++;
        }
    }
}

void sigma_delta_modulate(const short *pcm_in, unsigned int n_samples, unsigned char *pdm_out)
{
    sigma_delta_modulate_n(pcm_in, n_samples, pdm_out, 256);
}