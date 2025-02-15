#include "qpdf-pdftopdf-processor-private.h"
#include <stdio.h>
#include <stdarg.h>
#include "cupsfilters/debug-internal.h"
#include <stdexcept>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include "qpdf-tools-private.h"
#include "qpdf-xobject-private.h"
#include "qpdf-pdftopdf-private.h"
#include "pdftopdf-private.h"

// Use: content.append(debug_box(pe.sub,xpos,ypos));
static std::string debug_box(const _cfPDFToPDFPageRect &box,float xshift,float yshift) // {{{
{
  return std::string("q 1 w 0.1 G\n ")+
    QUtil::double_to_string(box.left+xshift)+" "+QUtil::double_to_string(box.bottom+yshift)+" m  "+
    QUtil::double_to_string(box.right+xshift)+" "+QUtil::double_to_string(box.top+yshift)+" l "+"S \n "+

    QUtil::double_to_string(box.right+xshift)+" "+QUtil::double_to_string(box.bottom+yshift)+" m  "+
    QUtil::double_to_string(box.left+xshift)+" "+QUtil::double_to_string(box.top+yshift)+" l "+"S \n "+

    QUtil::double_to_string(box.left+xshift)+" "+QUtil::double_to_string(box.bottom+yshift)+"  "+
    QUtil::double_to_string(box.right-box.left)+" "+QUtil::double_to_string(box.top-box.bottom)+" re "+"S Q\n";
}
// }}}

_cfPDFToPDFQPDFPageHandle::_cfPDFToPDFQPDFPageHandle(QPDFObjectHandle page,int orig_no) // {{{
  : page(page),
    no(orig_no),
    rotation(ROT_0)
{
}
// }}}

_cfPDFToPDFQPDFPageHandle::_cfPDFToPDFQPDFPageHandle(QPDF *pdf,float width,float height) // {{{
  : no(0),
    rotation(ROT_0)
{
  DEBUG_assert(pdf);
  page=QPDFObjectHandle::parse(
    "<<"
    "  /Type /Page"
    "  /Resources <<"
    "    /XObject null "
    "  >>"
    "  /MediaBox null "
    "  /Contents null "
    ">>");
  page.replaceKey("/MediaBox",_cfPDFToPDFMakeBox(0,0,width,height));
  page.replaceKey("/Contents",QPDFObjectHandle::newStream(pdf));
  // xobjects: later (in get())
  content.assign("q\n");  // TODO? different/not needed

  page=pdf->makeIndirectObject(page); // stores *pdf
}
// }}}

// Note: _cfPDFToPDFProcessor always works with "/Rotate"d and "/UserUnit"-scaled pages/coordinates/..., having 0,0 at left,bottom of the TrimBox
_cfPDFToPDFPageRect _cfPDFToPDFQPDFPageHandle::get_rect() const // {{{
{
  page.assertInitialized();
  _cfPDFToPDFPageRect ret=_cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));
  ret.translate(-ret.left,-ret.bottom);
  ret.rotate_move(_cfPDFToPDFGetRotate(page),ret.width,ret.height);
  ret.scale(_cfPDFToPDFGetUserUnit(page));
  return ret;
}
// }}}

bool _cfPDFToPDFQPDFPageHandle::is_existing() const // {{{
{
  page.assertInitialized();
  return content.empty();
}
// }}}

QPDFObjectHandle _cfPDFToPDFQPDFPageHandle::get() // {{{
{
  QPDFObjectHandle ret=page;
  if (!is_existing()) { // finish up page
    page.getKey("/Resources").replaceKey("/XObject",QPDFObjectHandle::newDictionary(xobjs));
    content.append("Q\n");
    page.getKey("/Contents").replaceStreamData(content,QPDFObjectHandle::newNull(),QPDFObjectHandle::newNull());
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(rotation));
  } else {
    pdftopdf_rotation_e rot=_cfPDFToPDFGetRotate(page)+rotation;
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(rot));
  }
  page=QPDFObjectHandle(); // i.e. uninitialized
  return ret;
}
// }}}

// TODO: we probably need a function "ungetRect()"  to transform to page/form space
// TODO: as member
static _cfPDFToPDFPageRect ungetRect(_cfPDFToPDFPageRect rect,const _cfPDFToPDFQPDFPageHandle &ph,pdftopdf_rotation_e rotation,QPDFObjectHandle page)
{
  _cfPDFToPDFPageRect pg1=ph.get_rect();
  _cfPDFToPDFPageRect pg2=_cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));

  // we have to invert /Rotate, /UserUnit and the left,bottom (TrimBox) translation
  //_cfPDFToPDFRotationDump(rotation);
  //_cfPDFToPDFRotationDump(_cfPDFToPDFGetRotate(page));
  rect.width=pg1.width;
  rect.height=pg1.height;
  //std::swap(rect.width,rect.height);
  //rect.rotate_move(-rotation,rect.width,rect.height);

  rect.rotate_move(-_cfPDFToPDFGetRotate(page),pg1.width,pg1.height);
  rect.scale(1.0/_cfPDFToPDFGetUserUnit(page));

  //  _cfPDFToPDFPageRect pg2=_cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));
  rect.translate(pg2.left,pg2.bottom);
  //rect.dump();

  return rect;
}

// TODO FIXME rotations are strange  ... (via ungetRect)
// TODO? for non-existing (either drop comment or facility to create split streams needed)
void _cfPDFToPDFQPDFPageHandle::add_border_rect(const _cfPDFToPDFPageRect &_rect,pdftopdf_border_type_e border,float fscale) // {{{
{
  DEBUG_assert(is_existing());
  DEBUG_assert(border!=pdftopdf_border_type_e::NONE);

  // straight from pstops
  const double lw=(border&THICK)?0.5:0.24;
  double line_width=lw*fscale;
  double margin=2.25*fscale;
  // (PageLeft+margin,PageBottom+margin) rect (PageRight-PageLeft-2*margin,...)   ... for nup>1: PageLeft=0,etc.
  //  if (double)  margin+=2*fscale ...rect...

  _cfPDFToPDFPageRect rect=ungetRect(_rect,*this,rotation,page);

  DEBUG_assert(rect.left<=rect.right);
  DEBUG_assert(rect.bottom<=rect.top);

  std::string boxcmd="q\n";
  boxcmd+="  "+QUtil::double_to_string(line_width)+" w 0 G \n";
  boxcmd+="  "+QUtil::double_to_string(rect.left+margin)+" "+QUtil::double_to_string(rect.bottom+margin)+"  "+
    QUtil::double_to_string(rect.right-rect.left-2*margin)+" "+QUtil::double_to_string(rect.top-rect.bottom-2*margin)+" re S \n";
  if (border&TWO) {
    margin+=2*fscale;
    boxcmd+="  "+QUtil::double_to_string(rect.left+margin)+" "+QUtil::double_to_string(rect.bottom+margin)+"  "+
      QUtil::double_to_string(rect.right-rect.left-2*margin)+" "+QUtil::double_to_string(rect.top-rect.bottom-2*margin)+" re S \n";
  }
  boxcmd+="Q\n";

  // if (!is_existing()) {
  //   // TODO: only after
  //   return;
  // }
  
  DEBUG_assert(page.getOwningQPDF()); // existing pages are always indirect
#ifdef DEBUG  // draw it on top
  static const char *pre="%pdftopdf q\n"
    "q\n",
    *post="%pdftopdf Q\n"
    "Q\n";

  QPDFObjectHandle stm1=QPDFObjectHandle::newStream(page.getOwningQPDF(),pre),
    stm2=QPDFObjectHandle::newStream(page.getOwningQPDF(),std::string(post)+boxcmd);

  page.addPageContents(stm1,true); // before
  page.addPageContents(stm2,false); // after
#else
  QPDFObjectHandle stm=QPDFObjectHandle::newStream(page.getOwningQPDF(),boxcmd);
  page.addPageContents(stm,true); // before
#endif
}
// }}}
/*
 *  This crop function is written for print-scaling=fill option.
 *  Trim Box is used for trimming the page in required size.
 *  scale tells if we need to scale input file.
 */
pdftopdf_rotation_e _cfPDFToPDFQPDFPageHandle::crop(const _cfPDFToPDFPageRect &cropRect,pdftopdf_rotation_e orientation,pdftopdf_rotation_e param_orientation,pdftopdf_position_e xpos,pdftopdf_position_e ypos,bool scale,bool autorotate,pdftopdf_doc_t *doc)
{
  page.assertInitialized();
  pdftopdf_rotation_e save_rotate = _cfPDFToPDFGetRotate(page);
  if(orientation==ROT_0||orientation==ROT_180)
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(ROT_90));
  else
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(ROT_0));

  _cfPDFToPDFPageRect currpage= _cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));
  double width = currpage.right-currpage.left;
  double height = currpage.top-currpage.bottom;
  double pageWidth = cropRect.right-cropRect.left;
  double pageHeight = cropRect.top-cropRect.bottom;
  double final_w,final_h;   //Width and height of cropped image.

  pdftopdf_rotation_e pageRot = _cfPDFToPDFGetRotate(page);
  if ((autorotate &&
       (((pageRot == ROT_0 || pageRot == ROT_180) &&
	 pageWidth <= pageHeight) ||
	((pageRot == ROT_90 || pageRot == ROT_270) &&
	 pageWidth > pageHeight))) ||
      (!autorotate &&
       (param_orientation == ROT_90 || param_orientation == ROT_270)))
  {
    std::swap(pageHeight,pageWidth);
  }
  if(scale)
  {
    if(width*pageHeight/pageWidth<=height)
    {
      final_w = width;
      final_h = width*pageHeight/pageWidth;
    }
    else{
      final_w = height*pageWidth/pageHeight;
      final_h = height;
    }
  }
  else
  {
    final_w = pageWidth;
    final_h = pageHeight;
  }
  if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_DEBUG,
	      "cfFilterPDFToPDF: After Cropping: %lf %lf %lf %lf",
	      width,height,final_w,final_h);
  double posw = (width-final_w)/2,
        posh = (height-final_h)/2;
  // posw, posh : pdftopdf_position_e along width and height respectively.
  // Calculating required position.  
  if(xpos==pdftopdf_position_e::LEFT)        
    posw =0;
  else if(xpos==pdftopdf_position_e::RIGHT)
    posw*=2;
  
  if(ypos==pdftopdf_position_e::TOP)
    posh*=2;
  else if(ypos==pdftopdf_position_e::BOTTOM)
    posh=0;

  // making _cfPDFToPDFPageRect for cropping.
  currpage.left += posw;
  currpage.bottom += posh;
  currpage.top =currpage.bottom+final_h;
  currpage.right=currpage.left+final_w;
  //Cropping.
  // TODO: Borders are covered by the image. buffer space?
  page.replaceKey("/TrimBox",_cfPDFToPDFMakeBox(currpage.left,currpage.bottom,currpage.right,currpage.top));
  page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(save_rotate));
  return _cfPDFToPDFGetRotate(page);
}

bool _cfPDFToPDFQPDFPageHandle::is_landscape(pdftopdf_rotation_e orientation)
{
  page.assertInitialized();
  pdftopdf_rotation_e save_rotate = _cfPDFToPDFGetRotate(page);
  if(orientation==ROT_0||orientation==ROT_180)
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(ROT_90));
  else
    page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(ROT_0));

  _cfPDFToPDFPageRect currpage= _cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));
  double width = currpage.right-currpage.left;
  double height = currpage.top-currpage.bottom;
  page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(save_rotate));
  if(width>height)
    return true;
  return false;
}

// TODO: better cropping
// TODO: test/fix with qsub rotation
void _cfPDFToPDFQPDFPageHandle::add_subpage(const std::shared_ptr<_cfPDFToPDFPageHandle> &sub,float xpos,float ypos,float scale,const _cfPDFToPDFPageRect *crop) // {{{
{
  auto qsub=dynamic_cast<_cfPDFToPDFQPDFPageHandle *>(sub.get());
  DEBUG_assert(qsub);

  std::string xoname="/X"+QUtil::int_to_string((qsub->no!=-1)?qsub->no:++no);
  if (crop) {
    _cfPDFToPDFPageRect pg=qsub->get_rect(),tmp=*crop;
    // we need to fix a too small cropbox.
    tmp.width=tmp.right-tmp.left;
    tmp.height=tmp.top-tmp.bottom;
    tmp.rotate_move(-_cfPDFToPDFGetRotate(qsub->page),tmp.width,tmp.height); // TODO TODO (pg.width? / unneeded?)
    // TODO: better
    // TODO: we need to obey page./Rotate
    if (pg.width<tmp.width) {
      pg.right=pg.left+tmp.width;
    }
    if (pg.height<tmp.height) {
      pg.top=pg.bottom+tmp.height;
    }

    _cfPDFToPDFPageRect rect=ungetRect(pg,*qsub,ROT_0,qsub->page);

    qsub->page.replaceKey("/TrimBox",_cfPDFToPDFMakeBox(rect.left,rect.bottom,rect.right,rect.top));
    // TODO? do everything for cropping here?
  }
  xobjs[xoname]=_cfPDFToPDFMakeXObject(qsub->page.getOwningQPDF(),qsub->page); // trick: should be the same as page->getOwningQPDF() [only after it's made indirect]

  _cfPDFToPDFMatrix mtx;
  mtx.translate(xpos,ypos);
  mtx.scale(scale);
  mtx.rotate(qsub->rotation); // TODO? -sub.rotation ?  // TODO FIXME: this might need another translation!?
  if (crop) { // TODO? other technique: set trim-box before _cfPDFToPDFMakeXObject (but this modifies original page)
    mtx.translate(crop->left,crop->bottom);
    // crop->dump();
  }

  content.append("q\n  ");
  content.append(mtx.get_string()+" cm\n  ");
  if (crop) {
    content.append("0 0 "+QUtil::double_to_string(crop->right-crop->left)+" "+QUtil::double_to_string(crop->top-crop->bottom)+" re W n\n  ");
    //    content.append("0 0 "+QUtil::double_to_string(crop->right-crop->left)+" "+QUtil::double_to_string(crop->top-crop->bottom)+" re S\n  ");
  }
  content.append(xoname+" Do\n");
  content.append("Q\n");
}
// }}}

void _cfPDFToPDFQPDFPageHandle::mirror() // {{{
{
  _cfPDFToPDFPageRect orig=get_rect();

  if (is_existing()) {
    // need to wrap in XObject to keep patterns correct
    // TODO? refactor into internal ..._subpage fn ?
    std::string xoname="/X"+QUtil::int_to_string(no);

    QPDFObjectHandle subpage=get();  // this->page, with rotation

    // replace all our data
    *this=_cfPDFToPDFQPDFPageHandle(subpage.getOwningQPDF(),orig.width,orig.height);

    xobjs[xoname]=_cfPDFToPDFMakeXObject(subpage.getOwningQPDF(),subpage); // we can only now set this->xobjs

    // content.append(std::string("1 0 0 1 0 0 cm\n  ");
    content.append(xoname+" Do\n");

    DEBUG_assert(!is_existing());
  }

  static const char *pre="%pdftopdf cm\n";
  // Note: we don't change (TODO need to?) the media box
  std::string mrcmd("-1 0 0 1 "+
                    QUtil::double_to_string(orig.right)+" 0 cm\n");

  content.insert(0,std::string(pre)+mrcmd);
}
// }}}

void _cfPDFToPDFQPDFPageHandle::rotate(pdftopdf_rotation_e rot) // {{{
{
  rotation=rot; // "rotation += rot;" ?
}
// }}}

void _cfPDFToPDFQPDFPageHandle::add_label(const _cfPDFToPDFPageRect &_rect, const std::string label) // {{{
{
  DEBUG_assert(is_existing());

  _cfPDFToPDFPageRect rect = ungetRect (_rect, *this, rotation, page);

  assert (rect.left <= rect.right);
  assert (rect.bottom <= rect.top);

  // TODO: Only add in the font once, not once per page.
  QPDFObjectHandle font = page.getOwningQPDF()->makeIndirectObject
    (QPDFObjectHandle::parse(
      "<<"
      " /Type /Font"
      " /Subtype /Type1"
      " /Name /pagelabel-font"
      " /BaseFont /Helvetica" // TODO: support UTF-8 labels?
      ">>"));
  QPDFObjectHandle resources = page.getKey ("/Resources");
  QPDFObjectHandle rfont = resources.getKey ("/Font");
  rfont.replaceKey ("/pagelabel-font", font);

  double margin = 2.25;
  double height = 12;

  std::string boxcmd = "q\n";

  // White filled rectangle (top)
  boxcmd += "  1 1 1 rg\n";
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + margin) + " " +
    QUtil::double_to_string(rect.top - height - 2 * margin) + " " +
    QUtil::double_to_string(rect.right - rect.left - 2 * margin) + " " +
    QUtil::double_to_string(height + 2 * margin) + " re f\n";

  // White filled rectangle (bottom)
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + margin) + " " +
    QUtil::double_to_string(rect.bottom + height + margin) + " " +
    QUtil::double_to_string(rect.right - rect.left - 2 * margin) + " " +
    QUtil::double_to_string(height + 2 * margin) + " re f\n";

  // Black outline (top)
  boxcmd += "  0 0 0 RG\n";
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + margin) + " " +
    QUtil::double_to_string(rect.top - height - 2 * margin) + " " +
    QUtil::double_to_string(rect.right - rect.left - 2 * margin) + " " +
    QUtil::double_to_string(height + 2 * margin) + " re S\n";

  // Black outline (bottom)
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + margin) + " " +
    QUtil::double_to_string(rect.bottom + height + margin) + " " +
    QUtil::double_to_string(rect.right - rect.left - 2 * margin) + " " +
    QUtil::double_to_string(height + 2 * margin) + " re S\n";

  // Black text (top)
  boxcmd += "  0 0 0 rg\n";
  boxcmd += "  BT\n";
  boxcmd += "  /pagelabel-font 12 Tf\n";
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + 2 * margin) + " " +
    QUtil::double_to_string(rect.top - height - margin) + " Td\n";
  boxcmd += "  (" + label + ") Tj\n";
  boxcmd += "  ET\n";

  // Black text (bottom)
  boxcmd += "  BT\n";
  boxcmd += "  /pagelabel-font 12 Tf\n";
  boxcmd += "  " +
    QUtil::double_to_string(rect.left + 2 * margin) + " " +
    QUtil::double_to_string(rect.bottom + height + 2 * margin) + " Td\n";
  boxcmd += "  (" + label + ") Tj\n";
  boxcmd += "  ET\n";

  boxcmd += "Q\n";

  DEBUG_assert(page.getOwningQPDF()); // existing pages are always indirect
  static const char *pre="%pdftopdf q\n"
    "q\n",
    *post="%pdftopdf Q\n"
    "Q\n";

  QPDFObjectHandle stm1=QPDFObjectHandle::newStream(page.getOwningQPDF(),
						    std::string(pre)),
    stm2=QPDFObjectHandle::newStream(page.getOwningQPDF(),
				     std::string(post) + boxcmd);

  page.addPageContents(stm1,true); // before
  page.addPageContents(stm2,false); // after
}
// }}}

void _cfPDFToPDFQPDFPageHandle::debug(const _cfPDFToPDFPageRect &rect,float xpos,float ypos) // {{{
{
  DEBUG_assert(!is_existing());
  content.append(debug_box(rect,xpos,ypos));
}
// }}}

void _cfPDFToPDFQPDFProcessor::close_file() // {{{
{
  pdf.reset();
  hasCM=false;
}
// }}}

// TODO?  try/catch for PDF parsing errors?

bool _cfPDFToPDFQPDFProcessor::load_file(FILE *f,pdftopdf_doc_t *doc,pdftopdf_arg_ownership_e take,int flatten_forms) // {{{
{
  close_file();
  if (!f) {
    throw std::invalid_argument("load_file(NULL,...) not allowed");
  }
  try {
    pdf.reset(new QPDF);
  } catch (...) {
    if (take==CF_PDFTOPDF_TAKE_OWNERSHIP) {
      fclose(f);
    }
    throw;
  }
  switch (take) {
  case CF_PDFTOPDF_WILL_STAY_ALIVE:
    try {
      pdf->processFile("temp file",f,false);
    } catch (const std::exception &e) {
      if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: load_file failed: %s", e.what());
      return false;
    }
    break;
  case CF_PDFTOPDF_TAKE_OWNERSHIP:
    try {
      pdf->processFile("temp file",f,true);
    } catch (const std::exception &e) {
      if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: load_file failed: %s", e.what());
      return false;
    }
    break;
  case CF_PDFTOPDF_MUST_DUPLICATE:
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: load_file with CF_PDFTOPDF_MUST_DUPLICATE is not supported");
    return false;
  }
  start(flatten_forms);
  return true;
}
// }}}

bool _cfPDFToPDFQPDFProcessor::load_filename(const char *name,pdftopdf_doc_t *doc,int flatten_forms) // {{{
{
  close_file();
  try {
    pdf.reset(new QPDF);
    pdf->processFile(name);
  } catch (const std::exception &e) {
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: load_filename failed: %s",e.what());
    return false;
  }
  start(flatten_forms);
  return true;
}
// }}}

void _cfPDFToPDFQPDFProcessor::start(int flatten_forms) // {{{
{
  DEBUG_assert(pdf);

  if (flatten_forms) {
    QPDFAcroFormDocumentHelper afdh(*pdf);
    afdh.generateAppearancesIfNeeded();

    QPDFPageDocumentHelper dh(*pdf);
    dh.flattenAnnotations(an_print);
  }

  pdf->pushInheritedAttributesToPage();
  orig_pages=pdf->getAllPages();

  // remove them (just unlink, data still there)
  const int len=orig_pages.size();
  for (int iA=0;iA<len;iA++) {
    pdf->removePage(orig_pages[iA]);
  }

  // we remove stuff that becomes defunct (probably)  TODO
  pdf->getRoot().removeKey("/PageMode");
  pdf->getRoot().removeKey("/Outlines");
  pdf->getRoot().removeKey("/OpenAction");
  pdf->getRoot().removeKey("/PageLabels");
}
// }}}

bool _cfPDFToPDFQPDFProcessor::check_print_permissions(pdftopdf_doc_t *doc) // {{{
{
  if (!pdf) {
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: No PDF loaded");
    return false;
  }
  return pdf->allowPrintHighRes() || pdf->allowPrintLowRes(); // from legacy pdftopdf
}
// }}}

std::vector<std::shared_ptr<_cfPDFToPDFPageHandle>> _cfPDFToPDFQPDFProcessor::get_pages(pdftopdf_doc_t *doc) // {{{
{
  std::vector<std::shared_ptr<_cfPDFToPDFPageHandle>> ret;
  if (!pdf) {
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: No PDF loaded");
    DEBUG_assert(0);
    return ret;
  }
  const int len=orig_pages.size();
  ret.reserve(len);
  for (int iA=0;iA<len;iA++) {
    ret.push_back(std::shared_ptr<_cfPDFToPDFPageHandle>(new _cfPDFToPDFQPDFPageHandle(orig_pages[iA],iA+1)));
  }
  return ret;
}
// }}}

std::shared_ptr<_cfPDFToPDFPageHandle> _cfPDFToPDFQPDFProcessor::new_page(float width,float height,pdftopdf_doc_t *doc) // {{{
{
  if (!pdf) {
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: No PDF loaded");
    DEBUG_assert(0);
    return std::shared_ptr<_cfPDFToPDFPageHandle>();
  }
  return std::shared_ptr<_cfPDFToPDFQPDFPageHandle>(new _cfPDFToPDFQPDFPageHandle(pdf.get(),width,height));
  // return std::make_shared<_cfPDFToPDFQPDFPageHandle>(pdf.get(),width,height);
  // problem: make_shared not friend
}
// }}}

void _cfPDFToPDFQPDFProcessor::add_page(std::shared_ptr<_cfPDFToPDFPageHandle> page,bool front) // {{{
{
  DEBUG_assert(pdf);
  auto qpage=dynamic_cast<_cfPDFToPDFQPDFPageHandle *>(page.get());
  if (qpage) {
    pdf->addPage(qpage->get(),front);
  }
}
// }}}

#if 0
// we remove stuff now probably defunct  TODO
pdf->getRoot().removeKey("/PageMode");
pdf->getRoot().removeKey("/Outlines");
pdf->getRoot().removeKey("/OpenAction");
pdf->getRoot().removeKey("/PageLabels");
#endif

void _cfPDFToPDFQPDFProcessor::multiply(int copies,bool collate) // {{{
{
  DEBUG_assert(pdf);
  DEBUG_assert(copies>0);

  std::vector<QPDFObjectHandle> pages=pdf->getAllPages(); // need copy
  const int len=pages.size();

  if (collate) {
    for (int iA=1;iA<copies;iA++) {
      for (int iB=0;iB<len;iB++) {
        pdf->addPage(pages[iB].shallowCopy(),false);
      }
    }
  } else {
    for (int iB=0;iB<len;iB++) {
      for (int iA=1;iA<copies;iA++) {
        pdf->addPageAt(pages[iB].shallowCopy(),false,pages[iB]);
      }
    }
  }
}
// }}}

// TODO? elsewhere?
void _cfPDFToPDFQPDFProcessor::auto_rotate_all(bool dst_lscape,pdftopdf_rotation_e normal_landscape) // {{{
{
  DEBUG_assert(pdf);

  const int len=orig_pages.size();
  for (int iA=0;iA<len;iA++) {
    QPDFObjectHandle page=orig_pages[iA];

    pdftopdf_rotation_e src_rot=_cfPDFToPDFGetRotate(page);

    // copy'n'paste from _cfPDFToPDFQPDFPageHandle::get_rect
    _cfPDFToPDFPageRect ret=_cfPDFToPDFGetBoxAsRect(_cfPDFToPDFGetTrimBox(page));
    // ret.translate(-ret.left,-ret.bottom);
    ret.rotate_move(src_rot,ret.width,ret.height);
    // ret.scale(_cfPDFToPDFGetUserUnit(page));

    const bool src_lscape=(ret.width>ret.height);
    if (src_lscape!=dst_lscape) {
      pdftopdf_rotation_e rotation=normal_landscape;
      // TODO? other rotation direction, e.g. if (src_rot==ROT_0)&&(param.orientation==ROT_270) ... etc.
      // rotation=ROT_270;

      page.replaceOrRemoveKey("/Rotate",_cfPDFToPDFMakeRotate(src_rot+rotation));
    }
  }
}
// }}}

#include "qpdf-cm-private.h"

// TODO
void _cfPDFToPDFQPDFProcessor::add_cm(const char *defaulticc,const char *outputicc) // {{{
{
  DEBUG_assert(pdf);

  if (_cfPDFToPDFHasOutputIntent(*pdf)) {
    return; // nothing to do
  }

  QPDFObjectHandle srcicc=_cfPDFToPDFSetDefaultICC(*pdf,defaulticc); // TODO? rename to putDefaultICC?
  _cfPDFToPDFAddDefaultRGB(*pdf,srcicc);

  _cfPDFToPDFAddOutputIntent(*pdf,outputicc);

  hasCM=true;
}
// }}}

void _cfPDFToPDFQPDFProcessor::set_comments(const std::vector<std::string> &comments) // {{{
{
  extraheader.clear();
  const int len=comments.size();
  for (int iA=0;iA<len;iA++) {
    DEBUG_assert(comments[iA].at(0)=='%');
    extraheader.append(comments[iA]);
    extraheader.push_back('\n');
  }
}
// }}}

void _cfPDFToPDFQPDFProcessor::emit_file(FILE *f,pdftopdf_doc_t *doc,pdftopdf_arg_ownership_e take) // {{{
{
  if (!pdf) {
    return;
  }
  QPDFWriter out(*pdf);
  switch (take) {
  case CF_PDFTOPDF_WILL_STAY_ALIVE:
    out.setOutputFile("temp file",f,false);
    break;
  case CF_PDFTOPDF_TAKE_OWNERSHIP:
    out.setOutputFile("temp file",f,true);
    break;
  case CF_PDFTOPDF_MUST_DUPLICATE:
    if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_ERROR,
        "cfFilterPDFToPDF: emit_file with CF_PDFTOPDF_MUST_DUPLICATE is not supported");
    return;
  }
  if (hasCM) {
    out.setMinimumPDFVersion("1.4");
  } else {
    out.setMinimumPDFVersion("1.2");
  }
  if (!extraheader.empty()) {
    out.setExtraHeaderText(extraheader);
  }
  out.setPreserveEncryption(false);
  out.write();
}
// }}}

void _cfPDFToPDFQPDFProcessor::emit_filename(const char *name,pdftopdf_doc_t *doc) // {{{
{
  if (!pdf) {
    return;
  }
  // special case: name==NULL -> stdout
  QPDFWriter out(*pdf,name);
  if (hasCM) {
    out.setMinimumPDFVersion("1.4");
  } else {
    out.setMinimumPDFVersion("1.2");
  }
  if (!extraheader.empty()) {
    out.setExtraHeaderText(extraheader);
  }
  out.setPreserveEncryption(false);
  std::vector<QPDFObjectHandle> pages=pdf->getAllPages();
  int len=pages.size();
  if (len)
  out.write();
  else
  if (doc->logfunc) doc->logfunc(doc->logdata, CF_LOGLEVEL_DEBUG,
	      "cfFilterPDFToPDF: No pages left, outputting empty file.");
}
// }}}

// TODO:
//   loadPDF();   success?

bool _cfPDFToPDFQPDFProcessor::has_acro_form() // {{{
{
  if (!pdf) {
    return false;
  }
  QPDFObjectHandle root=pdf->getRoot();
  if (!root.hasKey("/AcroForm")) {
    return false;
  }
  return true;
}
// }}}
