# Stock DNA tables

Verbatim rows extracted from the retail client's `libconfig_c.dat`
(`DNA_SkinInfo`, `DNA_ManeInfo`, `DNA_TailInfo`). They are the reference
`tools/check_dna_vs_stock.py` compares `resources/config/game/horses/appearance.yaml`
against, and they are vendored here on purpose: the oracle has to run inside the
image build, where the research directory does not exist. Do not "tidy" the
columns — the parser reads them by header name.
