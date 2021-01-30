// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#define fSPOwnedExtent          (1<<0)
#define fSPAvailExtent          (1<<1)
#define fSPExtentList           (1<<2)
#define fSPReservedExtent       (1<<3)
#define fSPShelvedExtent        (1<<4)

#define FSPOwnedExtent( fSPExtents )        ( (fSPExtents) & fSPOwnedExtent )
#define FSPAvailExtent( fSPExtents )        ( (fSPExtents) & fSPAvailExtent )
#define FSPExtentList( fSPExtents )         ( (fSPExtents) & fSPExtentList )
#define FSPReservedExtent( fSPExtents )     ( (fSPExtents) & fSPReservedExtent )
#define FSPShelvedExtent( fSPExtents )      ( (fSPExtents) & fSPShelvedExtent )


const BYTE  fSPHMultipleExtent      = 0x01;
const BYTE  fSPHNonUnique           = 0x02;
const BYTE  fSPHv2                  = 0x04;

#include <pshpack1.h>
PERSISTED
class SPACE_HEADER
{

    public:
        SPACE_HEADER();
        ~SPACE_HEADER();

    private:
        UnalignedLittleEndian< CPG >        m_cpgPrimary;
        UnalignedLittleEndian< PGNO >       m_pgnoParent;

        union
        {
            ULONG               m_ulSPHFlags;
            BYTE                m_rgbSPHFlags[4];
        };

        union
        {
            UINT                m_rgbitAvail : 32;
            PGNO                m_pgnoOE;
        };

    public:

        BOOL Fv1() const;
        BOOL Fv2() const;
        CPG CpgPrimary() const;
        CPG CpgLastAlloc() const;
        PGNO PgnoParent() const;
        BOOL FSingleExtent() const;
        BOOL FMultipleExtent() const;
        BOOL FUnique() const;
        BOOL FNonUnique() const;
        UINT RgbitAvail() const;
        PGNO PgnoOE() const;
        PGNO PgnoAE() const;

        VOID SetCpgPrimary( const CPG cpg );
        VOID SetCpgLast( const CPG cpgLast );
        VOID SetPgnoParent( const PGNO pgno );
        VOID SetMultipleExtent();
        VOID SetNonUnique();
        VOID SetRgbitAvail( const UINT rgbit );
        VOID SetPgnoOE( const PGNO pgno );

};
#include <poppack.h>


INLINE SPACE_HEADER::SPACE_HEADER()
{
    m_ulSPHFlags = 0;
    Assert( FSingleExtent() );
    Assert( FUnique() );
}
INLINE SPACE_HEADER::~SPACE_HEADER()
{
#ifdef DEBUG
    m_cpgPrimary    = 0xFFFFFFF;
    m_pgnoParent    = 0xFFFFFFF;
    m_ulSPHFlags    = 0xFFFFFFF;
    m_pgnoOE        = 0xFFFFFFF;
#endif
}


INLINE BOOL SPACE_HEADER::Fv1() const                       { return !( m_rgbSPHFlags[0] & fSPHv2 ); }
INLINE BOOL SPACE_HEADER::Fv2() const                       { return ( m_rgbSPHFlags[0] & fSPHv2 ); }
INLINE CPG SPACE_HEADER::CpgPrimary() const             { Assert( Fv1() ); return m_cpgPrimary; }
INLINE CPG SPACE_HEADER::CpgLastAlloc() const               { Assert( Fv2() );  return m_cpgPrimary; }
INLINE PGNO SPACE_HEADER::PgnoParent() const                { return m_pgnoParent; }
INLINE BOOL SPACE_HEADER::FSingleExtent() const             { return !( m_rgbSPHFlags[0] & fSPHMultipleExtent ); }
INLINE BOOL SPACE_HEADER::FMultipleExtent() const           { return ( m_rgbSPHFlags[0] & fSPHMultipleExtent ); }
INLINE BOOL SPACE_HEADER::FUnique() const                   { return !( m_rgbSPHFlags[0] & fSPHNonUnique ); }
INLINE BOOL SPACE_HEADER::FNonUnique() const                { return ( m_rgbSPHFlags[0] & fSPHNonUnique ); }
INLINE UINT SPACE_HEADER::RgbitAvail() const                { Assert( FSingleExtent() ); return (UINT)ReverseBytesOnBE( m_rgbitAvail ); }
INLINE PGNO SPACE_HEADER::PgnoOE() const                    { PGNO pgnoOE = PGNO(*(UnalignedLittleEndian< PGNO > *)(&m_pgnoOE)); Assert( FMultipleExtent() ); return pgnoOE; }
INLINE PGNO SPACE_HEADER::PgnoAE() const                    { PGNO pgnoOE = PGNO(*(UnalignedLittleEndian< PGNO > *)(&m_pgnoOE)); Assert( FMultipleExtent() ); return pgnoOE + 1; }

INLINE VOID SPACE_HEADER::SetCpgPrimary( const CPG cpg )    { m_cpgPrimary = cpg; }
INLINE VOID SPACE_HEADER::SetPgnoParent( const PGNO pgno )  { m_pgnoParent = pgno; }
INLINE VOID SPACE_HEADER::SetMultipleExtent()               { m_rgbSPHFlags[0] |= fSPHMultipleExtent; }
INLINE VOID SPACE_HEADER::SetNonUnique()                    { m_rgbSPHFlags[0] |= fSPHNonUnique; }
INLINE VOID SPACE_HEADER::SetRgbitAvail( const UINT rgbit ) { m_rgbitAvail = ReverseBytesOnBE( rgbit ); }
INLINE VOID SPACE_HEADER::SetPgnoOE( const PGNO pgno )      { *(UnalignedLittleEndian< PGNO > *)(&m_pgnoOE) = pgno; }
INLINE VOID SPACE_HEADER::SetCpgLast( const CPG cpgLast )
{
    if ( Fv1() )
    {
        m_rgbSPHFlags[0] |= fSPHv2;
    }
    Assert( Fv2() );
    m_cpgPrimary = cpgLast;
}

const ULONG cnodeSPBuildAvailExtCacheThreshold      = 16;

ERR ErrSPInit();
VOID SPTerm();

ERR ErrSPInitFCB( _Inout_ FUCB * const pfucb );
ERR ErrSPDeferredInitFCB( _Inout_ FUCB * const pfucb );
ERR ErrSPGetLastPgno( _Inout_ PIB * ppib, _In_ const IFMP ifmp, _Out_ PGNO * ppgno );
ERR ErrSPGetLastExtent( _Inout_ PIB * ppib, _In_ const IFMP ifmp, _Out_ EXTENTINFO * pextinfo );

const BOOL  fSPNoFlags              = 0x00000000;
const BOOL  fSPNewFDP               = 0x00000001;
const BOOL  fSPMultipleExtent       = 0x00000002;
const BOOL  fSPUnversionedExtent    = 0x00000004;
const BOOL  fSPSplitting            = 0x00000008;
const BOOL  fSPNonUnique            = 0x00000010;
const BOOL  fSPOriginatingRequest   = 0x00000020;
const BOOL  fSPContinuous           = 0x00000040;
const BOOL  fSPNewExtent            = 0x00000080;
const BOOL  fSPUseActiveReserve     = 0x00000100;
const BOOL  fSPExactExtent          = 0x00000200;
const BOOL  fSPSelfReserveExtent    = 0x00000400;

#define fMaskSPGetExt       ( fSPNewFDP | fSPMultipleExtent | fSPUnversionedExtent | fSPNonUnique )
#define fMaskSPGetPage      ( fSPContinuous | fSPNewExtent | fSPUseActiveReserve | fSPExactExtent )

ERR ErrSPCreate(
    PIB         *ppib,
    const IFMP  ifmp,
    const PGNO  pgnoParent,
    const PGNO  pgnoFDP,
    const CPG   cpgOwned,
    const ULONG fSPFlags,
    const ULONG fPageFlags,
    OBJID       *pobjidFDP );

ERR ErrSPBurstSpaceTrees( FUCB *pfucb );

ERR ErrSPGetExt(
    FUCB        *pfucb,
    PGNO        pgnoParentFDP,
    CPG         *pcpgReq,
    CPG         cpgMin,
    PGNO        *ppgnoFirst,
    ULONG       fSPFlags = 0,
    UINT        fPageFlags = 0,
    OBJID       *pobjidFDP = NULL );

ERR ErrSPGetPage(
    __inout FUCB *          pfucb,
    __in    const PGNO      pgnoLast,
    __in    const ULONG     fSPAllocFlags,
    __out   PGNO *          ppgnoAlloc
    );

ERR ErrSPCaptureSnapshot( FUCB* const pfucb, const PGNO pgnoFirst, const CPG cpgSize );

ERR ErrSPFreeExt( FUCB* const pfucb, const PGNO pgnoFirst, const CPG cpgSize, const CHAR* const szTag );

ERR ErrSPTryCoalesceAndFreeAvailExt( FUCB* const pfucb, const PGNO pgnoInExtent, BOOL* const pfCoalesced );

ERR ErrSPShelvePage( PIB* const ppib, const IFMP  ifmp, const PGNO pgno );

ERR ErrSPUnshelveShelvedPagesBelowEof( PIB* const ppib, const IFMP ifmp );

ERR ErrSPReclaimSpaceLeaks( PIB* const ppib, const IFMP ifmp );

ERR ErrSPFreeFDP(
    PIB         *ppib,
    FCB         *pfcbFDPToFree,
    const PGNO  pgnoFDPParent,
    const BOOL  fPreservePrimaryExtent = fFalse );

ERR ErrSPGetDatabaseInfo(
    PIB         *ppib,
    const IFMP  ifmp,
    __out_bcount(cbMax) BYTE        *pbResult,
    const ULONG cbMax,
    const ULONG fSPExtents,
    bool fUseCachedResult = 0,
    CPRINTF * const pcprintf = NULL );

ERR ErrSPGetInfo(
            PIB         *ppib,
            const IFMP  ifmp,
            FUCB        *pfucb,
            __out_bcount(cbMax) BYTE        *pbResult,
            const ULONG cbMax,
            const ULONG fSPExtents,
            CPRINTF * const pcprintf = NULL );
ERR ErrSPGetExtentInfo(
    _Inout_ PIB *       ppib,
    _In_ const IFMP     ifmp,
    _Inout_ FUCB *      pfucb,
    _In_ const ULONG    fSPExtents,
    _Out_ ULONG *       pulExtentList,
    _Deref_post_cap_(*pulExtentList) BTREE_SPACE_EXTENT_INFO ** pprgExtentList );

ERR ErrSPTrimRootAvail(
    _In_ PIB *ppib,
    _In_ const IFMP ifmp,
    _In_ CPRINTF * const pcprintf,
    _Out_opt_ CPG * const pcpgTrimmed = NULL );

typedef enum
{
    sdrNone,
    sdrFailed,
    sdrNoLowAvailSpace,
    sdrPageNotMovable,
    sdrTimeout,
    sdrNoSmallSpaceBurst,
    sdrMSysObjIdsNotReady,
    sdrReachedSizeLimit,
    sdrUnexpected,
} ShrinkDoneReason;

ERR ErrSPShrinkTruncateLastExtent(
    _In_ PIB* ppib,
    _In_ const IFMP ifmp,
    _In_ CPRINTF* const pcprintfShrinkTraceRaw,
    _Inout_ HRT* const pdhrtExtMaint,
    _Inout_ HRT* const pdhrtFileTruncation,
    _Out_ PGNO* const ppgnoFirstFromLastExtentTruncated,
    _Out_ ShrinkDoneReason* const psdr );


typedef struct SpaceCatCtx
{
    FUCB* pfucbParent;
    FUCB* pfucb;
    FUCB* pfucbSpace;
    BOOKMARK_COPY* pbm;
} SpaceCatCtx;

VOID SPFreeSpaceCatCtx( _Inout_ SpaceCatCtx** const ppSpCatCtx );

ERR ErrSPGetSpaceCategory(
    _In_ PIB* const ppib,
    _In_ const IFMP ifmp,
    _In_ const PGNO pgno,
    _In_ const OBJID objidHint,
    _In_ const BOOL fRunFullSpaceCat,
    _Out_ OBJID* const pobjid,
    _Out_ SpaceCategoryFlags* const pspcatf,
    _Out_ SpaceCatCtx** const ppSpCatCtx );

ERR ErrSPGetSpaceCategory(
    _In_ PIB* const ppib,
    _In_ const IFMP ifmp,
    _In_ const PGNO pgno,
    _In_ const OBJID objidHint,
    _In_ const BOOL fRunFullSpaceCat,
    _Out_ OBJID* const pobjid,
    _Out_ SpaceCategoryFlags* const pspcatf );

ERR ErrSPGetSpaceCategoryRange(
    _In_ PIB* const ppib,
    _In_ const IFMP ifmp,
    _In_ const PGNO pgnoFirst,
    _In_ const PGNO pgnoLast,
    _In_ const BOOL fRunFullSpaceCat,
    _In_ const JET_SPCATCALLBACK pfnCallback,
    _In_opt_ VOID* const pvContext = NULL );

BOOL FSPSpaceCatStrictlyLeaf( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatStrictlyInternal( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatRoot( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatSplitBuffer( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatSmallSpace( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatSpaceOE( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatSpaceAE( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatAnySpaceTree( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatAvailable( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatIndeterminate( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatInconsistent( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatLeaked( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatNotOwned( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatNotOwnedEof( const SpaceCategoryFlags spcatf );
BOOL FSPSpaceCatUnknown( const SpaceCategoryFlags spcatf );
BOOL FSPValidSpaceCategoryFlags( const SpaceCategoryFlags spcatf );



ERR ErrSPCreateMultiple(
    FUCB        *pfucb,
    const PGNO  pgnoParent,
    const PGNO  pgnoFDP,
    const OBJID objidFDP,
    const PGNO  pgnoOE,
    const PGNO  pgnoAE,
    const PGNO  pgnoPrimary,
    const CPG   cpgPrimary,
    const BOOL  fUnique,
    const ULONG fPageFlags );
ERR ErrSPIOpenAvailExt( PIB *ppib, FCB *pfcb, FUCB **ppfucbAE );
ERR ErrSPIOpenOwnExt( PIB *ppib, FCB *pfcb, FUCB **ppfucbOE );
ERR ErrSPReserveSPBufPagesForSpaceTree( FUCB *pfucb, FUCB *pfucbSpace, FUCB *pfucbParent );
ERR ErrSPReserveSPBufPages(
    FUCB* const pfucbSpace,
    const BOOL fAddlReserveForPageMoveOE,
    const BOOL fAddlReserveForPageMoveAE );
ERR ErrSPReplaceSPBuf(
    FUCB* const pfucb,
    FUCB* const pfucbParent,
    const PGNO pgnoReplace );

ERR ErrSPDummyUpdate( FUCB * pfucb );

CPG CpgSPIGetNextAlloc(
    __in const FCB_SPACE_HINTS * const  pfcbsh,
    __in const CPG                      cpgPrevious
    );

ERR ErrSPExtendDB(
    PIB *       ppib,
    const IFMP  ifmp,
    const CPG   cpgSEMin,
    PGNO        *ppgnoSELast,
    const BOOL  fPermitAsyncExtension );

ERR ErrSPREPAIRValidateSpaceNode(
    __in const  KEYDATAFLAGS * pkdf,
    __out       PGNO *          ppgnoLast,
    __out       CPG *           pcpgExtent,
    __out       PCWSTR *        pwszPoolName );

ERR ErrSPTrimDBTaskInit( const IFMP ifmp );
VOID SPTrimDBTaskStop( INST * pinst, const WCHAR * wszDatabaseFullName = NULL );

INLINE BOOL FSPExpectedError( const ERR err )
{
    BOOL    fExpectedErr;

    switch ( err )
    {
        case JET_errSuccess:
        case JET_errDiskFull:
        case JET_errLogDiskFull:
        case JET_errOutOfDatabaseSpace:
        case errSPNoSpaceBelowShrinkTarget:
        case_AllDatabaseStorageCorruptionErrs:
            fExpectedErr = fTrue;
            break;

        case JET_errOutOfCursors:
        case JET_errOutOfMemory:
        case JET_errOutOfBuffers:
        case JET_errTransactionTooLong:
        case JET_errDiskIO:
        case JET_errLogWriteFail:
            fExpectedErr = fTrue;
            break;

        default:
            fExpectedErr = fFalse;
    }

    return fExpectedErr;
}

BOOL FSPIsRootSpaceTree( const FUCB * const pfucb );

PGNO PgnoSPIParentFDP( FUCB *pfucb );

const INT   cSecFrac                = 4;

const PGNO  pgnoSysMax              = (0x7fefffff);

const CPG   cpgSingleExtentMin      = 1;
const CPG   cpgMultipleExtentMin    = 3;

const CPG   cpgTableMin             = cpgSingleExtentMin * 2;

const ULONG cbSecondaryExtentMost = ( 100 * 1024 * 1024 );

