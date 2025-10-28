const char *
xal_odf_dinode_format_str(int val);

int
xal_odf_dinode_pp(void *buf);

int
xal_odf_sb_pp(void *buf);

int
xal_odf_agf_pp(void *buf);

int
xal_odf_agi_pp(void *buf);

int
xal_odf_agfl_pp(void *buf);

int
xal_odf_btree_iab3_sfmt_pp(struct xal_odf_btree_sfmt *iab3);

int
xal_odf_inobt_rec_pp(struct xal_odf_inobt_rec *rec);

int
xal_ag_pp(struct xal_ag *ag);
