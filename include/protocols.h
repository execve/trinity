#ifndef _PROTOCOLS_H
#define _PROTOCOLS_H 1

extern const char * get_proto_name(unsigned int proto);
extern void find_specific_proto(const char *protoarg);
extern void parse_exclude_protos(const char *arg);

#endif /* _PROTOCOLS_H */
