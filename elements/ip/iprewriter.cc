/*
 * iprewriter.{cc,hh} -- rewrites packet source and destination
 * Max Poletto, Eddie Kohler
 *
 * Copyright (c) 2000 Massachusetts Institute of Technology.
 *
 * This software is being provided by the copyright holders under the GNU
 * General Public License, either version 2 or, at your discretion, any later
 * version. For more information, see the `COPYRIGHT' file in the source
 * distribution.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include "iprewriter.hh"
#include "elements/ip/iprwpatterns.hh"
#include "click_ip.h"
#include "click_tcp.h"
#include "click_udp.h"
#include "confparse.hh"
#include "straccum.hh"
#include "error.hh"

#include <limits.h>

#ifdef CLICK_LINUXMODULE
extern "C" {
#include <asm/softirq.h>
#include <net/sock.h>
#ifdef HAVE_TCP_PROT
extern struct proto tcp_prot;
#endif
}
#endif


//
// IPRewriter::Mapping
//

IPRewriter::Mapping::Mapping(const IPFlowID &in, const IPFlowID &out,
			     Pattern *pat, int output, bool is_reverse)
  : _mapto(out), _out(output), _used(false), _is_reverse(is_reverse),
    _pat(pat), _pat_prev(0), _pat_next(0)
{
  // set checksum increments
  const unsigned short *source_words = (const unsigned short *)&in;
  const unsigned short *dest_words = (const unsigned short *)&_mapto;
  unsigned increment = 0;
  for (int i = 0; i < 4; i++) {
    increment += (~ntohs(source_words[i]) & 0xFFFF);
    increment += ntohs(dest_words[i]);
  }
  while (increment >> 16)
    increment = (increment & 0xFFFF) + (increment >> 16);
  _ip_csum_incr = increment;
  
  for (int i = 4; i < 6; i++) {
    increment += (~ntohs(source_words[i]) & 0xFFFF);
    increment += ntohs(dest_words[i]);
  }
  while (increment >> 16)
    increment = (increment & 0xFFFF) + (increment >> 16);
  _udp_csum_incr = increment;
}

bool
IPRewriter::Mapping::make_pair(const IPFlowID &inf, const IPFlowID &outf,
			       Pattern *pattern, int foutput, int routput,
			       Mapping **in_map, Mapping **out_map)
{
  Mapping *im = new Mapping(inf, outf, pattern, foutput, false);
  if (!im)
    return false;
  Mapping *om = new Mapping(outf.rev(), inf.rev(), pattern, routput, true);
  if (!om) {
    delete im;
    return false;
  }
  im->_reverse = om;
  om->_reverse = im;
  *in_map = im;
  *out_map = om;
  return true;
}

void
IPRewriter::Mapping::apply(WritablePacket *p)
{
  click_ip *iph = p->ip_header();
  assert(iph);
  
  // IP header
  iph->ip_src = _mapto.saddr();
  iph->ip_dst = _mapto.daddr();

  unsigned sum = (~ntohs(iph->ip_sum) & 0xFFFF) + _ip_csum_incr;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  iph->ip_sum = ~htons(sum);

  // UDP/TCP header
  if (iph->ip_p == IP_PROTO_TCP) {
    click_tcp *tcph = reinterpret_cast<click_tcp *>(p->transport_header());
    tcph->th_sport = _mapto.sport();
    tcph->th_dport = _mapto.dport();
    unsigned sum2 = (~ntohs(tcph->th_sum) & 0xFFFF) + _udp_csum_incr;
    while (sum2 >> 16)
      sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    tcph->th_sum = ~htons(sum2);
  } else {
    click_udp *udph = reinterpret_cast<click_udp *>(p->transport_header());
    udph->uh_sport = _mapto.sport();
    udph->uh_dport = _mapto.dport();
    if (udph->uh_sum) {		// 0 checksum is no checksum
      unsigned sum2 = (~ntohs(udph->uh_sum) & 0xFFFF) + _udp_csum_incr;
      while (sum2 >> 16)
	sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
      udph->uh_sum = ~htons(sum2);
    }
  }
  
  mark_used();
}

inline void
IPRewriter::Mapping::pat_unlink()
{
  _pat_next->_pat_prev = _pat_prev;
  _pat_prev->_pat_next = _pat_next;
}

//
// IPRewriter::Pattern
//

IPRewriter::Pattern::Pattern(const IPAddress &saddr, int sportl, int sporth,
			     const IPAddress &daddr, int dport)
  : _saddr(saddr), _sportl(sportl), _sporth(sporth), _daddr(daddr),
    _dport(dport), _rover(0)
{
}

int
IPRewriter::Pattern::parse(const String &conf, Pattern **pstore,
			   Element *e, ErrorHandler *errh)
{
  Vector<String> words;
  cp_spacevec(conf, words);

  // check for IPRewriterPatterns reference
  if (words.size() == 1) {
    if (Pattern *p = IPRewriterPatterns::find(e, cp_unquote(words[0]), errh)) {
      *pstore = p;
      return 0;
    } else
      return -1;
  }

  // otherwise, pattern definition
  if (words.size() != 4)
    return errh->error("bad pattern spec: should be `NAME FOUTPUT ROUTPUT' or\n`SADDR SPORT DADDR DPORT FOUTPUT ROUTPUT'");
  
  IPAddress saddr, daddr;
  int sportl, sporth, dport;
  
  if (words[0] == "-")
    saddr = 0;
  else if (!cp_ip_address(words[0], saddr))
    return errh->error("bad source address `%s' in pattern spec", words[0].cc());
  
  if (words[1] == "-")
    sportl = sporth = 0;
  else {
    String rest;
    if (!cp_integer(words[1], &sportl, &rest))
      return errh->error("bad source port `%s' in pattern spec", words[1].cc());
    if (rest) {
      if (!cp_integer(rest, &sporth) || sporth > 0)
	return errh->error("bad source port `%s' in pattern spec", words[1].cc());
      sporth = -sporth;
    } else
      sporth = sportl;
  }
  if (sportl > sporth || sportl < 0 || sporth > USHRT_MAX)
    return errh->error("source port(s) %d-%d out of range in pattern spec", sportl, sporth);

  if (words[2] == "-")
    daddr = 0;
  else if (!cp_ip_address(words[2], daddr))
    return errh->error("bad destination address `%s' in pattern spec", words[2].cc());
  
  if (words[3] == "-")
    dport = 0;
  else if (!cp_integer(words[3], &dport))
    return errh->error("bad destination port `%s' in pattern spec", words[3].cc());
  if (dport < 0 || dport > USHRT_MAX)
    return errh->error("destination port %d out of range in pattern spec", dport);

  *pstore = new Pattern(saddr, sportl, sporth, daddr, dport);
  return 0;
}

int
IPRewriter::Pattern::parse_with_ports(const String &conf, Pattern **pstore,
				      int *fport_store, int *rport_store,
				      Element *e, ErrorHandler *errh)
{
  Vector<String> words;
  cp_spacevec(conf, words);
  int fport, rport;

  if (words.size() <= 2
      || !cp_integer(words[words.size() - 2], &fport)
      || !cp_integer(words[words.size() - 1], &rport))
    return errh->error("bad forward and/or reverse ports in pattern spec");
  words.resize(words.size() - 2);

  // check for IPRewriterPatterns reference
  Pattern *p;
  if (parse(cp_unspacevec(words), &p, e, errh) >= 0) {
    *pstore = p;
    *fport_store = fport;
    *rport_store = rport;
    return 0;
  } else
    return -1;
}

static bool
possible_conflict_port(IPAddress a1, int p1l, int p1h,
		       IPAddress a2, int p2l, int p2h)
{
  if (a1 && a2 && a1 != a2)
    return false;
  if (!p1l || !p2l)
    return true;
  if ((p1l <= p2l && p2l <= p1h) || (p2l <= p1l && p1l <= p2h))
    return true;
  return false;
}

bool
IPRewriter::Pattern::possible_conflict(const Pattern &o) const
{
  return possible_conflict_port(_saddr, _sportl, _sporth,
				o._saddr, o._sportl, o._sporth)
    && possible_conflict_port(_daddr, _dport, _dport,
			      o._daddr, o._dport, o._dport);
}

bool
IPRewriter::Pattern::definite_conflict(const Pattern &o) const
{
  if (_saddr && _sportl && _daddr && _dport
      && _saddr == o._saddr && _daddr == o._daddr && _dport == o._dport
      && ((_sportl <= o._sportl && o._sporth <= _sporth)
	  || (o._sportl <= _sportl && _sporth <= o._sporth)))
    return true;
  else
    return false;
}

inline unsigned short
IPRewriter::Pattern::find_sport()
{
  if (_sportl == _sporth || !_rover)
    return htons((short)_sportl);

  // search for empty port number starting at `_rover'
  Mapping *r = _rover;
  unsigned short this_sport = ntohs(r->sport());
  do {
    Mapping *next = r->pat_next();
    unsigned short next_sport = ntohs(next->sport());
    if (next_sport > this_sport + 1)
      goto found;
    else if (next_sport <= this_sport) {
      if (this_sport < _sporth)
	goto found;
      else if (next_sport > _sportl) {
	this_sport = _sportl - 1;
	goto found;
      }
    }
    r = next;
    this_sport = next_sport;
  } while (r != _rover);

  // nothing found
  return 0;

 found:
  _rover = r;
  return htons(this_sport + 1);
}

bool
IPRewriter::Pattern::create_mapping(const IPFlowID &in, int fport, int rport,
				    Mapping **fmap, Mapping **rmap)
{
  unsigned short new_sport;
  if (!_sportl)
    new_sport = in.sport();
  else {
    new_sport = find_sport();
    if (!new_sport)
      return false;
  }

  // convoluted logic avoids internal compiler errors in gcc-2.95.2
  unsigned short new_dport = (_dport ? htons((short)_dport) : in.dport());
  IPFlowID out(_saddr, new_sport, _daddr, new_dport);
  if (!_saddr) out.set_saddr(in.saddr());
  if (!_daddr) out.set_daddr(in.daddr());

  if (Mapping::make_pair(in, out, this, fport, rport, fmap, rmap)) {
    (*fmap)->pat_insert_after(_rover);
    _rover = *fmap;
    return true;
  } else
    return false;
}

inline void
IPRewriter::Pattern::mapping_freed(Mapping *m)
{
  if (_rover == m) {
    _rover = m->pat_next();
    if (_rover == m)
      _rover = 0;
  }
  m->pat_unlink();
}

String
IPRewriter::Pattern::s() const
{
  String saddr, sport, daddr, dport;
  saddr = _saddr ? _saddr.s() : String("-");
  daddr = _daddr ? _daddr.s() : String("-");
  dport = _dport ? (String)_dport : String("-");
  if (!_sporth)
    sport = "-";
  else if (_sporth == _sportl)
    sport = String(_sporth);
  else
    sport = String(_sportl) + "-" + String(_sporth);
  return saddr + ":" + sport + " / " + daddr + ":" + dport;
}

//
// IPMapper
//

void
IPMapper::mapper_patterns(Vector<IPRewriter::Pattern *> &, IPRewriter *) const
{
}

IPRewriter::Mapping *
IPMapper::get_map(bool, const IPFlowID &, IPRewriter *)
{
  return 0;
}

//
// IPRewriter
//

IPRewriter::IPRewriter()
  : _tcp_map(0), _udp_map(0), _timer(this)
{
}

IPRewriter::~IPRewriter()
{
  assert(!_timer.scheduled());
}

void
IPRewriter::notify_noutputs(int n)
{
  set_noutputs(n < 1 ? 1 : n);
}

int
IPRewriter::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  if (conf.size() == 0)
    return errh->error("too few arguments; expected `IPRewriter(INPUTSPEC, ...)'");
  set_ninputs(conf.size());

  // parse arguments
  int before = errh->nerrors();
  for (int i = 0; i < conf.size(); i++) {
    String word, rest;
    if (!cp_word(conf[i], &word, &rest)) {
      errh->error("input %d spec is empty", i);
      continue;
    }
    cp_eat_space(rest);

    InputSpec is;
    is.kind = INPUT_SPEC_DROP;
    
    if (word == "nochange") {
      int outnum = 0;
      if ((rest && !cp_integer(rest, &outnum))
	  || (outnum < 0 || outnum >= noutputs()))
	errh->error("bad input %d spec; expected `nochange [OUTPUT]'", i);
      is.kind = INPUT_SPEC_NOCHANGE;
      is.u.output = outnum;
      
    } else if (word == "drop") {
      if (rest)
	errh->error("bad input %d spec; expected `drop'", i);

    } else if (word == "pattern") {
      if (Pattern::parse_with_ports(rest, &is.u.pattern.p, &is.u.pattern.fport,
				    &is.u.pattern.rport, this, errh) >= 0) {
	is.u.pattern.p->use();
	is.kind = INPUT_SPEC_PATTERN;
      }

    } else if (Element *e = cp_element(word, this, 0)) {
      IPMapper *mapper = (IPMapper *)e->cast("IPMapper");
      if (rest || !mapper)
	errh->error("bad input %d spec; expected `ELEMENTNAME'", i);
      else {
	is.kind = INPUT_SPEC_MAPPER;
	is.u.mapper = mapper;
      }
      
    } else
      errh->error("unknown input %d spec `%s'", i, word.cc());

    _input_specs.push_back(is);
  }


  // ... all_pat ...
  
  return (errh->nerrors() == before ? 0 : -1);
}

int
IPRewriter::initialize(ErrorHandler *errh)
{
  _timer.attach(this);
  _timer.schedule_after_ms(GC_INTERVAL_SEC * 1000);
#if defined(CLICK_LINUXMODULE) && !defined(HAVE_TCP_PROT)
  errh->message
    ("The kernel does not export the symbol `tcp_prot', so I cannot remove\n"
     "stale mappings. Apply the Click kernel patch to fix this problem.");
#endif
#ifndef CLICK_LINUXMODULE
  errh->message("can't remove stale mappings at userlevel");
  click_chatter("Patterns:\n%s", dump_patterns().cc());
#endif

  return 0;
}

void
IPRewriter::uninitialize()
{
  _timer.unschedule();

  clear_map(_tcp_map);
  clear_map(_udp_map);

  for (int i = 0; i < _input_specs.size(); i++)
    if (_input_specs[i].kind == INPUT_SPEC_PATTERN)
      _input_specs[i].u.pattern.p->unuse();
}

void
IPRewriter::mark_live_tcp()
{
#if defined(CLICK_LINUXMODULE) && defined(HAVE_TCP_PROT)
#if 0
  start_bh_atomic();

  for (struct sock *sp = tcp_prot.sklist_next;
       sp != (struct sock *)&tcp_prot;
       sp = sp->sklist_next) {
    // socket port numbers are already in network byte order
    IPFlowID c(sp->rcv_saddr, sp->sport, sp->daddr, sp->dport);
    if (Mapping *m = _tcp_map.find(c))
      m->mark_used();
  }

  end_bh_atomic();
#endif
#endif
}

void
IPRewriter::clear_map(HashMap<IPFlowID, Mapping *> &h)
{
  Vector<Mapping *> to_free;
  
  int i = 0;
  IPFlowID flow;
  Mapping *m;
  while (h.each(i, flow, m))
    if (m && m->is_forward())
      to_free.push_back(m);

  for (i = 0; i < to_free.size(); i++) {
    Mapping *m = to_free[i];
    if (Pattern *p = m->pattern())
      p->mapping_freed(m);
    delete m->reverse();
    delete m;
  }
}

void
IPRewriter::clean_map(HashMap<IPFlowID, Mapping *> &h)
{
  Vector<Mapping *> to_free;
  
  int i = 0;
  IPFlowID flow;
  Mapping *m;
  while (h.each(i, flow, m))
    if (m) {
      if (!m->used() && !m->reverse()->used() && !m->is_reverse())
	to_free.push_back(m);
      else
	m->clear_used();
    }
  
  for (i = 0; i < to_free.size(); i++) {
    m = to_free[i];
    if (Pattern *p = m->pattern())
      p->mapping_freed(m);
    h.insert(m->reverse()->flow_id().rev(), 0);
    h.insert(m->flow_id().rev(), 0);
    delete m->reverse();
    delete m;
  }
}

void
IPRewriter::clean()
{
  clean_map(_tcp_map);
  clean_map(_udp_map);
}

void
IPRewriter::run_scheduled()
{
#if defined(CLICK_LINUXMODULE) && defined(HAVE_TCP_PROT)
  mark_live_tcp();
#endif
  clean();
  _timer.schedule_after_ms(GC_INTERVAL_SEC * 1000);
}

void
IPRewriter::install(bool is_tcp, Mapping *forward, Mapping *reverse)
{
  IPFlowID forward_flow_id = reverse->flow_id().rev();
  IPFlowID reverse_flow_id = forward->flow_id().rev();
  if (is_tcp) {
    _tcp_map.insert(forward_flow_id, forward);
    _tcp_map.insert(reverse_flow_id, reverse);
  } else {
    _udp_map.insert(forward_flow_id, forward);
    _udp_map.insert(reverse_flow_id, reverse);
  }
}

void
IPRewriter::push(int port, Packet *p_in)
{
  WritablePacket *p = p_in->uniqueify();
  IPFlowID flow(p);
  click_ip *iph = p->ip_header();
  assert(iph->ip_p == IP_PROTO_TCP || iph->ip_p == IP_PROTO_UDP);
  bool tcp = iph->ip_p == IP_PROTO_TCP;

  Mapping *m = (tcp ? _tcp_map.find(flow) : _udp_map.find(flow));
  
  if (!m) {			// create new mapping
    const InputSpec &is = _input_specs[port];
    switch (is.kind) {

     case INPUT_SPEC_NOCHANGE:
      output(is.u.output).push(p);
      return;

     case INPUT_SPEC_DROP:
      break;

     case INPUT_SPEC_PATTERN: {
       Pattern *pat = is.u.pattern.p;
       int fport = is.u.pattern.fport;
       int rport = is.u.pattern.rport;
       Mapping *reverse;
       if (pat->create_mapping(flow, fport, rport, &m, &reverse))
	 install(tcp, m, reverse);
       break;
     }

     case INPUT_SPEC_MAPPER:
      m = is.u.mapper->get_map(tcp, flow, this);
      break;
      
    }
    if (!m) {
      p->kill();
      return;
    }
  }
  
  m->apply(p);
  output(m->output()).push(p);
}


String
IPRewriter::dump_table()
{
  StringAccum tcps, udps;
  int i = 0;
  IPFlowID2 in;
  Mapping *m;
  while (_tcp_map.each(i, in, m))
    if (m && !m->is_reverse())
      tcps << in.s() << " => " << m->flow_id().s() << " [" << String(m->output()) << "]\n";
  i = 0;
  while (_udp_map.each(i, in, m))
    if (m && !m->is_reverse())
      udps << in.s() << " => " << m->flow_id().s() << " [" << String(m->output()) << "]\n";
  if (tcps.length() && udps.length())
    return "TCP:\n" + tcps.take_string() + "\nUDP:\n" + udps.take_string();
  else if (tcps.length())
    return "TCP:\n" + tcps.take_string();
  else if (udps.length())
    return "UDP:\n" + udps.take_string();
  else
    return String();
}

String
IPRewriter::dump_patterns()
{
  String s;
  for (int i = 0; i < _input_specs.size(); i++)
    if (_input_specs[i].kind == INPUT_SPEC_PATTERN)
      s += _input_specs[i].u.pattern.p->s() + "\n";
  return s;
}

static String
table_dump(Element *f, void *)
{
  IPRewriter *r = (IPRewriter *)f;
  return r->dump_table();
}

static String
patterns_dump(Element *f, void *)
{
  IPRewriter *r = (IPRewriter *)f;
  return r->dump_patterns();
}

void
IPRewriter::add_handlers()
{
  add_read_handler("mappings", table_dump, (void *)0);
  add_read_handler("patterns", patterns_dump, (void *)0);
}

ELEMENT_REQUIRES(IPRewriterPatterns)
EXPORT_ELEMENT(IPRewriter)

#include "hashmap.cc"
#include "vector.cc"
