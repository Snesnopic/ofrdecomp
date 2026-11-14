# Documentation index

- [status.md](status.md): what works and how each part was verified. **Start here.**
- [format.md](format.md): container and bitstream format, block handling, seeking, tags.
- [optimfrog_re_knowledge.md](optimfrog_re_knowledge.md): overview of the algorithms (range coder,
  entropy decoders, predictors, post-processors).
- [pred3_analysis.md](pred3_analysis.md): cascade NLMS predictor and LDLT final combiner
  (`pred_type=2` and `3`).
- [ent3_analysis.md](ent3_analysis.md): ACM entropy coder (`entropy_type=3`).
- [post2_remap_analysis.md](post2_remap_analysis.md): value-remap post-processor (`post_type=2`).
- [float_analysis.md](float_analysis.md): IEEE Float (`.off`) sample transform and DETA sub-block.
- [dualstream_analysis.md](dualstream_analysis.md): DualStream (`.ofs`, `pred_type=4`).

The official documentation, changelog, SDK and licence terms are on
[losslessaudio.org](http://losslessaudio.org/). They are not copied here: this repository only
holds text written for this project.
