// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "std.hxx"
#include "_dump.hxx"


PERFInstanceLiveTotal<> cFMPagesWrittenAsync;
PERFInstanceLiveTotal<> cFMPagesWrittenSync;
PERFInstanceDelayedTotal<> cFMPagesDirty;
PERFInstanceDelayedTotal<> cFMPagesTotal;

LONG LFMPagesWrittenAsyncCEFLPv( LONG iInstance, void* pvBuf )
{
    if ( pvBuf != NULL )
    {
        cFMPagesWrittenAsync.PassTo( iInstance, pvBuf );
    }

    return 0;
}

LONG LFMPagesWrittenSyncCEFLPv( LONG iInstance, void* pvBuf )
{
    if ( pvBuf != NULL )
    {
        cFMPagesWrittenSync.PassTo( iInstance, pvBuf );
    }

    return 0;
}

LONG LFMDirtyPagesCEFLPv( LONG iInstance, void* pvBuf )
{
    if ( pvBuf != NULL )
    {
        cFMPagesDirty.PassTo( iInstance, pvBuf );
    }

    return 0;
}

LONG LFMTotalPagesCEFLPv( LONG iInstance, void* pvBuf )
{
    if ( pvBuf != NULL )
    {
        cFMPagesTotal.PassTo( iInstance, pvBuf );
    }

    return 0;
}



const WCHAR* const CFlushMap::s_wszFmFileExtension = L".jfm";

INT CFlushMap::IGetPageLockSubrank_( const FMPGNO fmpgno )
{
    const INT subrank = lMax - 2 - fmpgno;

    Assert( ( subrank >= 0 ) && ( subrank != CLockDeadlockDetectionInfo::subrankNoDeadlock ) );

    return subrank;
}

BOOL CFlushMap::FIsFmHeader_( const FMPGNO fmpgno )
{
    const BOOL fIsFmHeader = ( fmpgno == s_fmpgnoHdr );
    return fIsFmHeader;
}

QWORD CFlushMap::OffsetOfFmPgno_( const FMPGNO fmpgno )
{
    return ( ( fmpgno + 1 ) * s_cbFlushMapPageOnDisk );
}

FMPGNO CFlushMap::FmPgnoOfOffset_( const QWORD ibOffset )
{
    Assert( ( ibOffset % s_cbFlushMapPageOnDisk ) == 0 );
    return ( (FMPGNO)( ibOffset / s_cbFlushMapPageOnDisk ) - 1 );
}

BOOL CFlushMap::FCheckForDbFmConsistency_(
    const SIGNATURE* const psignDbHdrFlushFromDb,
    const SIGNATURE* const psignFlushMapHdrFlushFromDb,
    const SIGNATURE* const psignDbHdrFlushFromFm,
    const SIGNATURE* const psignFlushMapHdrFlushFromFm )
{
    return ( ( !memcmp( psignDbHdrFlushFromDb, psignDbHdrFlushFromFm, sizeof( SIGNATURE ) ) &&
            FSIGSignSet( psignDbHdrFlushFromDb ) ) ||
            ( !memcmp( psignFlushMapHdrFlushFromDb, psignFlushMapHdrFlushFromFm, sizeof( SIGNATURE ) ) &&
            FSIGSignSet( psignFlushMapHdrFlushFromDb ) ) );
}

ERR CFlushMap::ErrGetFmPathFromDbPath(
    _Out_writes_z_(OSFSAPI_MAX_PATH) WCHAR* const wszFmPath,
    _In_z_ const WCHAR* const wszDbPath )
{
    ERR err = JET_errSuccess;
    IFileSystemAPI* pfsapi = NULL;
    WCHAR wszDbFolder[ IFileSystemAPI::cchPathMax ] = { L"\0" };
    WCHAR wszDbFileName[ IFileSystemAPI::cchPathMax ] = { L"\0" };
    WCHAR wszDbExtension[ IFileSystemAPI::cchPathMax ] = { L"\0" };
    DWORD cchDbPath = 0;
    DWORD cchDbExtension = 0;
    DWORD cchFmExtension = 0;

    Call( ErrOSFSCreate( &pfsapi ) );
    Call( pfsapi->ErrPathParse( wszDbPath, wszDbFolder, wszDbFileName, wszDbExtension ) );

    cchDbPath = (DWORD)wcslen( wszDbPath );
    cchDbExtension = (DWORD)wcslen( wszDbExtension );
    cchFmExtension = (DWORD)wcslen( s_wszFmFileExtension );
    if ( ( cchDbPath - cchDbExtension + cchFmExtension + 1 ) > IFileSystemAPI::cchPathMax )
    {
        Error( ErrERRCheck( JET_errInvalidPath ) );
    }
    else
    {
        Call( pfsapi->ErrPathBuild( wszDbFolder, wszDbFileName, s_wszFmFileExtension, wszFmPath ) );
    }

HandleError:

    delete pfsapi;
    return err;
}

ERR CFlushMap::ErrChecksumFmPage_(
    void* const pv,
    const FMPGNO fmpgno,
    const BOOL fCorrectError,
    XECHECKSUM* const pchecksumPersisted,
    XECHECKSUM* const pchecksumCalculated )
{
    Assert( pv != NULL );
    
    if ( !CPAGE::FPageIsInitialized( pv, s_cbFlushMapPageOnDisk ) )
    {
        return ErrERRCheck( JET_errPageNotInitialized );
    }

    PAGECHECKSUM checksumPersisted = 0;
    PAGECHECKSUM checksumCalculated = 0;
    BOOL fCorrectableError = fFalse;
    INT ibitCorrupted = 0;

    Unused( fCorrectableError );
    Unused( ibitCorrupted );

    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );
    ChecksumAndPossiblyFixPage(
        pv,
        s_cbFlushMapPageOnDisk,
        fFmHeaderPage ? flushmapHeaderPage : flushmapDataPage,
        fmpgno,
        !!fCorrectError,
        &checksumPersisted,
        &checksumCalculated,
        &fCorrectableError,
        &ibitCorrupted );

    if ( pchecksumPersisted != NULL )
    {
        *pchecksumPersisted = checksumPersisted.rgChecksum[ 0 ];
    }

    if ( pchecksumCalculated != NULL )
    {
        *pchecksumCalculated = checksumCalculated.rgChecksum[ 0 ];
    }

    if ( checksumPersisted != checksumCalculated )
    {
        return ErrERRCheck( JET_errReadVerifyFailure );
    }

    return JET_errSuccess;
}

CFMPG CFlushMap::CfmpgGetRequiredFmDataPageCount_( const PGNO pgnoReq )
{
    return roundupdiv( pgnoReq + 1, m_cDbPagesPerFlushMapPage );
}

CFMPG CFlushMap::CfmpgGetPreferredFmDataPageCount_( const PGNO pgnoReq )
{
    Assert( pgnoReq <= pgnoSysMax );


    const ULONG cfmpgRequiredFmDataPageCount = (ULONG)CfmpgGetRequiredFmDataPageCount_( pgnoReq );
    Assert( cfmpgRequiredFmDataPageCount > 0 );
    Assert( cfmpgRequiredFmDataPageCount <= (ULONG)lMax );
    ULONG cfmpgPreferredFmDataPageCount = cfmpgRequiredFmDataPageCount;

    const ULONG cfmpgWriteMax = roundupdiv( (ULONG)UlParam( JET_paramMaxCoalesceWriteSize ), s_cbFlushMapPageOnDisk );
    const ULONG cpgDbExtension = (ULONG)CpgGetDbExtensionSize_();

    if ( !m_fPersisted || ( cpgDbExtension == 0 ) )
    {
        goto Return;
    }


    const ULONG cpgActual = pgnoReq + 1;
    ULONG cpgEstimate = 10 * cpgDbExtension;

    if ( cpgEstimate > cpgActual )
    {
        cfmpgPreferredFmDataPageCount = CfmpgGetRequiredFmDataPageCount_( cpgEstimate - 1 );
        if ( cfmpgPreferredFmDataPageCount >= 2 )
        {
            cfmpgPreferredFmDataPageCount = roundup( cfmpgRequiredFmDataPageCount, cfmpgWriteMax );
        }

        goto Return;
    }

    cpgEstimate = 2 * cpgActual;
    cfmpgPreferredFmDataPageCount = CfmpgGetRequiredFmDataPageCount_( cpgEstimate - 1 );

    if ( cfmpgPreferredFmDataPageCount >= cfmpgWriteMax )
    {
        cfmpgPreferredFmDataPageCount = UlFunctionalMax( rounddn( cfmpgPreferredFmDataPageCount, cfmpgWriteMax ), cfmpgRequiredFmDataPageCount );
    }

Return:
    if ( ( cfmpgPreferredFmDataPageCount > cfmpgRequiredFmDataPageCount ) &&
        ( ( cfmpgPreferredFmDataPageCount - cfmpgRequiredFmDataPageCount ) > ( 4 * cfmpgWriteMax ) ) )
    {
        cfmpgPreferredFmDataPageCount = cfmpgRequiredFmDataPageCount + 4 * cfmpgWriteMax - 1;
    }

    Assert( cfmpgRequiredFmDataPageCount >= 1 );
    Assert( cfmpgPreferredFmDataPageCount >= cfmpgRequiredFmDataPageCount );

    return (CFMPG)UlFunctionalMin( cfmpgPreferredFmDataPageCount, (ULONG)CfmpgGetRequiredFmDataPageCount_( pgnoSysMax ) );
}

QWORD CFlushMap::CbGetRequiredFmFileSize_( const CFMPG cfmpgDataNeeded )
{
    return ( (QWORD)( ( cfmpgDataNeeded + 1 ) * s_cbFlushMapPageOnDisk ) );
}

ERR CFlushMap::ErrAttachFlushMap_()
{
    ERR err = JET_errSuccess;
    BOOL fFileExists = fFalse;
    BOOL fNewFile = fFalse;
    BOOL fFileValid = fFalse;
    const BOOL fCached = BoolParam( JET_paramEnableFileCache );
    const BOOL fWriteBack = fCached && !m_fReadOnly && !m_fRecoverable;
    const IFileAPI::FileModeFlags fmfInstCacheSettings = (
            ( m_fReadOnly ? IFileAPI::fmfReadOnly : IFileAPI::fmfNone ) |
            ( fCached ? IFileAPI::fmfCached : IFileAPI::fmfNone ) |
            ( fWriteBack ? IFileAPI::fmfLossyWriteBack : IFileAPI::fmfNone ) |
            ( ( !m_fReadOnly && !fCached && !fWriteBack && ( m_pinst != pinstNil ) && BoolParam( m_pinst, JET_paramUseFlushForWriteDurability ) ) ? IFileAPI::fmfStorageWriteBack : IFileAPI::fmfNone ) );
    WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ] = { L'\0' };
    const WCHAR* wszDeletedReason = L"";
    CFMPG cfmpgActual = 0;

    Assert( m_fPersisted );
    Expected( !m_fmdFmHdr.FValid() );

    err = ErrGetFmFilePath_( wszFmFilePath );
    if ( err == JET_errInvalidPath )
    {
        m_fPersisted = fFalse;
        err = JET_errSuccess;
    }
    else
    {
        Call( err );
    }

    if ( m_fPersisted )
    {
        const BOOL fCreateNew = !m_fDumpMode && ( m_fCreateNew || ( UlGetDbState_() == JET_dbstateJustCreated ) );
        
        Assert( m_pfsapi == NULL );
        if ( m_pinst == pinstNil )
        {
            Call( ErrOSFSCreate( Pfsconfig_(), &m_pfsapi ) );
        }
        IFileSystemAPI* const pfsapi = ( m_pinst != pinstNil ) ? m_pinst->m_pfsapi : m_pfsapi;
        Assert( pfsapi != NULL );
        
        fFileExists = ( ErrUtilPathExists( pfsapi, wszFmFilePath ) == JET_errSuccess );

        if ( fFileExists )
        {
            BOOL fSuccess = fFalse;
            wszDeletedReason = L"CreateNew";

            if ( !fCreateNew )
            {
                const ERR errOpen = ( m_pinst != pinstNil ) ?
                    (
                    CIOFilePerf::ErrFileOpen(
                        pfsapi,
                        m_pinst,
                        wszFmFilePath,
                        fmfInstCacheSettings | ( m_fReadOnly ? IFileAPI::fmfReadOnly : IFileAPI::fmfNone ),
                        iofileFlushMap,
                        QwGetEseFileId_(),
                        &m_pfapi )
                    ) :
                    (
                    pfsapi->ErrFileOpen(
                        wszFmFilePath,
                        ( fmfInstCacheSettings & ~IFileAPI::fmfStorageWriteBack ) | ( m_fReadOnly ? IFileAPI::fmfReadOnly : IFileAPI::fmfNone ),
                        &m_pfapi )
                    );

                wszDeletedReason = L"FileOpenFailed";
                fSuccess = ( errOpen >= JET_errSuccess );

                if ( ( m_fDumpMode && !fSuccess ) || ( errOpen == JET_errOutOfMemory ) )
                {
                    Call( errOpen );
                }
            }

            if ( fSuccess )
            {
                if ( m_pinst != pinstNil )
                {
                    Call( ErrUtilFlushFileBuffers( m_pfapi, iofrOpeningFileFlush ) );
                }
                AllowFlushMapIo_();
                m_fmdFmHdr.sxwl.AcquireWriteLatch();
                wszDeletedReason = L"ReadHdrFailed";
                const ERR errHdr = ErrSyncReadFmPage_( &m_fmdFmHdr );
                fSuccess = ( errHdr >= JET_errSuccess );
                m_fmdFmHdr.sxwl.ReleaseWriteLatch();

                if ( ( m_fDumpMode && !fSuccess ) || ( errHdr == JET_errOutOfMemory ) )
                {
                    Call( errHdr );
                }
            }

            if ( fSuccess && !m_fReadOnly )
            {
                m_fmdFmHdr.sxwl.AcquireWriteLatch();
                FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)( m_fmdFmHdr.pvWriteBuffer );
                LGIGetDateTime( &pfmfilehdr->logtimeAttach );
                pfmfilehdr->lGenMinRequired = LGetDbGenMinRequired_();
                pfmfilehdr->lGenMinRequiredTarget = 0;
                pfmfilehdr->ResetClean();
                m_fmdFmHdr.SetDirty( m_pinst );
                wszDeletedReason = L"WriteHdrFailed";
                fSuccess = ( ErrSyncWriteFmPage_( &m_fmdFmHdr ) >= JET_errSuccess );
                m_fmdFmHdr.sxwl.ReleaseExclusiveLatch();
            }

            if ( fSuccess )
            {
                wszDeletedReason = L"FfbHdrFailed";
                fSuccess = ( ErrUtilFlushFileBuffers( m_pfapi, iofrFmInit ) >= JET_errSuccess );
            }

            QWORD cbFmFileSizeActual = 0;

            if ( fSuccess )
            {
                wszDeletedReason = L"GetSizeFailed";
                fSuccess = ( m_pfapi->ErrSize( &cbFmFileSizeActual, IFileAPI::filesizeLogical ) >= JET_errSuccess );
            }

            if ( fSuccess )
            {
                wszDeletedReason = L"FileTooSmall";
                fSuccess = ( cbFmFileSizeActual >= s_cbFlushMapPageOnDisk );
            }

            if ( fSuccess )
            {
                fFileValid = fTrue;
                Assert( m_fmdFmHdr.FValid() );
                cfmpgActual = (CFMPG)( cbFmFileSizeActual / s_cbFlushMapPageOnDisk ) - 1;
                Assert( cfmpgActual >= 0 );
                cfmpgActual = max( cfmpgActual, 1 );

                if ( !m_fDumpMode )
                {
                    Call( ErrAllocateDescriptorsCapacity_( cfmpgActual ) );
                    Call( ErrAllocateFmDataPageCapacity_( cfmpgActual ) );
                    PrereadFlushMap_( cfmpgActual );
                }
            }
            else
            {
                if ( m_pfapi != NULL )
                {
                    WaitForFlushMapIoQuiesce_();
                    DisallowFlushMapIo_();
                    m_pfapi->SetNoFlushNeeded();
                    delete m_pfapi;
                    m_pfapi = NULL;
                }

                if ( !m_fReadOnly )
                {
                    if ( !fCreateNew )
                    {
                        LogEventFmFileDeleted_( wszFmFilePath, wszDeletedReason );
                    }
                    Call( pfsapi->ErrFileDelete( wszFmFilePath ) );
                    fFileExists = fFalse;
                }
            }
        }
        else if ( m_fDumpMode )
        {
            Error( ErrERRCheck( JET_errFileNotFound ) );
        }

        if ( !fFileExists && !m_fReadOnly )
        {
#ifndef ENABLE_MICROSOFT_MANAGED_DATACENTER_LEVEL_OPTICS
            if ( wszDeletedReason[0] != L'\0' )
#endif
            {
                LogEventFmFileCreated_( wszFmFilePath );
            }
            const ERR errT = ( m_pinst != pinstNil ) ?
                (
                CIOFilePerf::ErrFileCreate(
                    pfsapi,
                    m_pinst,
                    wszFmFilePath,
                    fmfInstCacheSettings | IFileAPI::fmfOverwriteExisting,
                    iofileFlushMap,
                    QwGetEseFileId_(),
                    &m_pfapi )
                ) :
                (
                pfsapi->ErrFileCreate(
                    wszFmFilePath,
                    fmfInstCacheSettings | IFileAPI::fmfOverwriteExisting,
                    &m_pfapi )
                );

            BOOL fSuccess = ( errT >= JET_errSuccess );

            if ( fSuccess )
            {
                fNewFile = fTrue;
                fFileExists = fTrue;
                AllowFlushMapIo_();
                m_fmdFmHdr.sxwl.AcquireWriteLatch();
                InitializeFmHdr_( &m_fmdFmHdr );
                ( (FMFILEHDR*)( m_fmdFmHdr.pvWriteBuffer ) )->ResetClean();
                m_fmdFmHdr.SetDirty( m_pinst );
                m_fmdFmHdr.SetValid();
                wszDeletedReason = L"WriteHdrFailed";
                fSuccess = ( ErrSyncWriteFmPage_( &m_fmdFmHdr ) >= JET_errSuccess );
                m_fmdFmHdr.sxwl.ReleaseExclusiveLatch();
            }

            if ( fSuccess )
            {
                wszDeletedReason = L"FfbHdrFailed";
                fSuccess = ( ErrUtilFlushFileBuffers( m_pfapi, iofrFmInit ) >= JET_errSuccess );
            }

            if ( fSuccess )
            {
                fFileValid = fTrue;
                cfmpgActual = 0;
            }
            else if ( fFileExists )
            {
                if ( m_pfapi != NULL )
                {
                    WaitForFlushMapIoQuiesce_();
                    DisallowFlushMapIo_();
                    m_pfapi->SetNoFlushNeeded();
                    delete m_pfapi;
                    m_pfapi = NULL;
                }

                LogEventFmFileDeleted_( wszFmFilePath, wszDeletedReason );
                Call( pfsapi->ErrFileDelete( wszFmFilePath ) );
                fFileExists = fFalse;
            }
        }

        if ( fFileValid )
        {
            if ( fNewFile )
            {
                Assert( !m_fReadOnly );
                Call( ErrSetFlushMapCapacity( 1 ) );
            }
        }
        else
        {
            m_fPersisted = fFalse;
        }
    }

    Assert( m_fmdFmHdr.FValid() || !fFileValid );
    Assert( ( m_pfapi != NULL ) || !fFileValid );
    Assert( fFileValid || ( m_pfapi == NULL ) );

HandleError:

    if ( err < JET_errSuccess )
    {
        LogEventFmFileAttachFailed_( wszFmFilePath, err );

        if ( m_pfapi != NULL )
        {
            WaitForFlushMapIoQuiesce_();
            DisallowFlushMapIo_();
            m_pfapi->SetNoFlushNeeded();
            delete m_pfapi;
            m_pfapi = NULL;
        }
    }

    return err;
}

void CFlushMap::TermFlushMap_()
{
    m_fGetSetAllowed = fFalse;

    if ( m_fPersisted )
    {
        Assert( ( m_pfapi != NULL ) || !m_fInitialized );

        if ( m_fInitialized )
        {
            if ( !m_fReadOnly )
            {
                Assert( m_sxwlSectionFlush.FNotOwner() );
                m_sxwlSectionFlush.AcquireWriteLatch();
                m_sxwlSectionFlush.ReleaseOwnership( CSXWLatch::iWriteGroup );
            }
            
            OnDebug( AssertFlushMapIoAllowed_() );
            WaitForFlushMapIoQuiesce_();
            DisallowFlushMapIo_();

            WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ] = { L'\0' };
            if ( m_pfapi->ErrPath( wszFmFilePath ) < JET_errSuccess )
            {
                wszFmFilePath[0] = L'\0';
            }

            if ( m_pinst != pinstNil )
            {
                (void)ErrUtilFlushFileBuffers( m_pfapi, iofrFmTerm );
            }
            else
            {
#ifndef ENABLE_JET_UNIT_TEST
#endif
                if ( m_pfapi )
                {
                    m_pfapi->SetNoFlushNeeded();
                }
            }

            delete m_pfapi;
            m_pfapi = NULL;
        }
        else
        {
            OnDebug( AssertFlushMapIoDisallowed_() );
        }

        Assert( m_sxwlSectionFlush.FWriteLatched() );
        Assert( m_sxwlSectionFlush.FNotOwner() );

        for ( FMPGNO fmpgno = 0; fmpgno < m_cfmpgAllocated; fmpgno++ )
        {
            FlushMapPageDescriptor* pfmd = NULL;
            CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
            pfmd->sxwl.AcquireWriteLatch();
            Assert( !pfmd->FReadInProgress() );
            Assert( !pfmd->FWriteInProgress() );
            pfmd->sxwl.ReleaseWriteLatch();
        }
    }
    else
    {
        OnDebug( AssertFlushMapIoDisallowed_() );
    }

    delete m_pfsapi;
    m_pfsapi = NULL;

#ifdef DEBUG
    if ( m_fPersisted && m_fCleanForTerm )
    {
        const FMPGNO fmpgnoUsedMax = m_cfmpgAllocated - 1;
        for ( FMPGNO fmpgno = s_fmpgnoHdr; fmpgno <= fmpgnoUsedMax; fmpgno++ )
        {
            FlushMapPageDescriptor* pfmd = NULL;
            CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
            AssertSz( pfmd->FAllocated(), "Page must be allocated." );
            AssertSz( pfmd->FValid(), "Page must be valid." );
            AssertSz( !pfmd->FDirty(), "Page must be clean." );
        }
    }
#endif

    Assert( !m_critFmdGrowCapacity.FAcquired() );
    Assert( !m_fmdFmHdr.sxwl.FLatched() );
    Assert( !m_fmdFmPg0.sxwl.FLatched() );
    
    if ( m_rgfmd != NULL )
    {
        Assert( m_cbfmdReserved > 0 );
        Assert( ( m_cbfmdCommitted > 0 ) || ( m_cfmdCommitted == 0 ) );

        for ( FMPGNO ifmd = 0; ifmd < m_cfmdCommitted; ifmd++ )
        {
            FlushMapPageDescriptor* const pfmd = &m_rgfmd[ ifmd ];

            Assert( !pfmd->sxwl.FLatched() );
            pfmd->ResetDirty( m_pinst );

            if ( pfmd->pv != NULL )
            {
                Assert( ifmd > 0 );
                Assert( ifmd < m_cfmpgAllocated );
                Assert( m_cbfmAllocated >= ( 1 * m_cbFmPageInMemory ) );
                FreeFmPage_( pfmd );
                m_cbfmAllocated -= m_cbFmPageInMemory;
            }

            pfmd->~FlushMapPageDescriptor();
        }

        OSMemoryPageFree( m_rgfmd );
        m_rgfmd = NULL;
        m_cfmdCommitted = 0;
        m_cbfmdReserved = 0;
        m_cbfmdCommitted = 0;
    }
    else
    {
        Assert( m_cfmpgAllocated <= 1 );
        Assert( m_cbfmdReserved == 0 );
        Assert( m_cbfmdCommitted == 0 );
        Assert( m_cfmdCommitted == 0 );
    }

    Assert( m_sxwlSectionFlush.FWriteLatched() );
    Assert( m_sxwlSectionFlush.FNotOwner() );

    if ( m_fmdFmPg0.pv != NULL )
    {
        Assert( m_cfmpgAllocated >= 1 );
        Assert( m_cbfmAllocated == ( 1 * m_cbFmPageInMemory ) );
        FreeFmPage_( &m_fmdFmPg0 );
        m_cbfmAllocated -= m_cbFmPageInMemory;
    }

    m_fmdFmPg0.ResetDirty( m_pinst );
    ( &m_fmdFmPg0 )->~FlushMapPageDescriptor();
    new( &m_fmdFmPg0 ) FlushMapPageDescriptor( 0 );
    Assert( m_cbfmAllocated == 0 );
    m_cbfmAllocated = 0;

    m_fmdFmHdr.ResetDirty( m_pinst );
    FreeFmPage_( &m_fmdFmHdr );
    ( &m_fmdFmHdr )->~FlushMapPageDescriptor();
    new( &m_fmdFmHdr ) FlushMapPageDescriptor( s_fmpgnoHdr );

    if ( m_pvSectionFlushAsyncWriteBuffer != NULL )
    {
        Assert( !m_fReadOnly );
        Assert( m_msSectionFlush.FEmpty() );
        OSMemoryPageFree( m_pvSectionFlushAsyncWriteBuffer );
        m_pvSectionFlushAsyncWriteBuffer = NULL;
        m_cbSectionFlushAsyncWriteBufferReserved = 0;
        m_cbSectionFlushAsyncWriteBufferCommitted = 0;
        m_ibSectionFlushAsyncWriteBufferNext = 0;
        m_fmpgnoSectionFlushFirst = s_fmpgnoUninit;
        m_fmpgnoSectionFlushLast = s_fmpgnoUninit;
        m_fmpgnoSectionFlushNext = s_fmpgnoUninit;
        m_fSectionCheckpointHeaderWrite = fFalse;
        m_errSectionFlushWriteLastError = JET_errSuccess;
        m_groupSectionFlushWrite = CMeteredSection::groupInvalidNil;
        m_cbLogGeneratedAtStartOfFlush = ullMax;
        m_cbLogGeneratedAtLastFlushProgress = ullMax;
    }

    m_lgenTargetedMax = 0;

    m_fPersisted = fFalse;
    m_fCreateNew = fFalse;
    m_fReadOnly = fFalse;
    m_fRecoverable = fFalse;
    m_fCleanForTerm = fFalse;
    if ( m_pinst != pinstNil )
    {
        PERFOpt( cFMPagesTotal.Add( m_pinst->m_iInstance, -m_cfmpgAllocated ) );
    }
    m_pinst = pinstNil;
    m_pfapi = NULL;
    m_fInitialized = fFalse;
    m_cfmpgAllocated = 0;
}

INLINE ERR CFlushMap::ErrGetDescriptorFromFmPgno_( const FMPGNO fmpgno, FlushMapPageDescriptor** const ppfmd )
{
    ERR err = JET_errSuccess;
    FlushMapPageDescriptor* pfmd = NULL;
    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );

    Assert( !m_fDumpMode );

    if ( ( fmpgno < 0 ) && !fFmHeaderPage )
    {
        Error( ErrERRCheck( JET_errInvalidParameter ) );
    }

    if ( fFmHeaderPage )
    {
        pfmd = &m_fmdFmHdr;
        goto HandleError;
    }

    if ( fmpgno == 0 )
    {
        pfmd = &m_fmdFmPg0;
        goto HandleError;
    }

    if ( fmpgno >= m_cfmdCommitted )
    {
        Error( ErrERRCheck( JET_errInvalidOperation ) );
    }

    pfmd = &m_rgfmd[ fmpgno ];

HandleError:

    if ( err >= JET_errSuccess )
    {
        *ppfmd = pfmd;
    }

    return err;
}

INLINE ERR CFlushMap::ErrGetDescriptorFromPgno_(
    const PGNO pgno,
    FlushMapPageDescriptor** const ppfmd )
{
    return ErrGetDescriptorFromFmPgno_( FmpgnoGetFmPgnoFromDbPgno( pgno ), ppfmd );
}

ERR CFlushMap::ErrAllocateDescriptorsCapacity_( const CFMPG cfmdNeeded )
{
    ERR err = JET_errSuccess;

    m_critFmdGrowCapacity.Enter();

    if ( cfmdNeeded < 2 )
    {
        goto HandleError;
    }

    if ( cfmdNeeded <= m_cfmdCommitted )
    {
        goto HandleError;
    }

    if ( m_rgfmd == NULL )
    {
        Assert( m_cbfmdReserved == 0 );
        Assert( m_cbfmdCommitted == 0 );
        Assert( m_cbfmAllocated <= ( 1 * m_cbFmPageInMemory ) );
        Assert( ( (BYTE*)&m_rgfmd[ 1 ] - (BYTE*)&m_rgfmd[ 0 ] ) == sizeof( FlushMapPageDescriptor ) );

        const CFMPG cfmpg = CfmpgGetPreferredFmDataPageCount_( pgnoSysMax );
        Assert( cfmpg > 0 );

        const DWORD cbfmdCommittedMax = (DWORD)roundup( cfmpg * sizeof( FlushMapPageDescriptor ), m_cbDescriptorPage );
        const DWORD cbfmdReserved = roundup( cbfmdCommittedMax, OSMemoryPageReserveGranularity() );
        Assert( ( cbfmdReserved & ( m_cbDescriptorPage - 1 ) ) == 0 );

        Alloc( m_rgfmd = (FlushMapPageDescriptor*)PvOSMemoryPageReserve( cbfmdReserved, NULL ) );
        m_cbfmdReserved = cbfmdReserved;
        m_cbfmdCommitted = 0;
    }

    const DWORD cbfmdCommitted = (DWORD)roundup( m_cfmdCommitted * sizeof( FlushMapPageDescriptor ), m_cbDescriptorPage );
    const DWORD cbfmdCommittedNeeded = (DWORD)roundup( cfmdNeeded * sizeof( FlushMapPageDescriptor ), m_cbDescriptorPage );
    Assert( cbfmdCommitted == m_cbfmdCommitted );
    Assert( cbfmdCommittedNeeded > cbfmdCommitted );
    Assert( ( ( cbfmdCommittedNeeded - cbfmdCommitted ) % m_cbDescriptorPage ) == 0 );
    Enforce( cbfmdCommittedNeeded <= m_cbfmdReserved );
    
    if ( !FOSMemoryPageCommit( (BYTE*)m_rgfmd + cbfmdCommitted, cbfmdCommittedNeeded - cbfmdCommitted ) )
    {
        Alloc( NULL );
    }

    const CFMPG cfmdCommittedNeeded = cbfmdCommittedNeeded / sizeof( FlushMapPageDescriptor );

    for ( FMPGNO ifmd = m_cfmdCommitted; ifmd < cfmdCommittedNeeded; ifmd++ )
    {
        new( &m_rgfmd[ ifmd ] ) FlushMapPageDescriptor( ifmd );
    }

    m_cbfmdCommitted = cbfmdCommittedNeeded;
    m_cfmdCommitted = cfmdCommittedNeeded;

HandleError:

    m_critFmdGrowCapacity.Leave();

    return err;
}

ERR CFlushMap::ErrAllocateFmDataPageCapacity_( const CFMPG cfmpgNeeded )
{
    ERR err = JET_errSuccess;

    m_critFmdGrowCapacity.Enter();

    if ( cfmpgNeeded <= m_cfmpgAllocated )
    {
        goto HandleError;
    }

    for ( FMPGNO fmpgno = m_cfmpgAllocated; fmpgno < cfmpgNeeded; fmpgno++ )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
        Call( ErrAllocateFmPage_( pfmd ) );
        m_cbfmAllocated += m_cbFmPageInMemory;
        m_cfmpgAllocated++;
        if ( m_pinst != pinstNil )
        {
            PERFOpt( cFMPagesTotal.Inc( m_pinst->m_iInstance ) );
        }
    }
    
HandleError:

    m_critFmdGrowCapacity.Leave();

    return err;
}

ERR CFlushMap::ErrAllocateFmPage_( FlushMapPageDescriptor* const pfmd )
{
    ERR err = JET_errSuccess;
    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();
    
    pfmd->sxwl.AcquireWriteLatch();
    Assert( !pfmd->FAllocated() );
    Assert( !pfmd->FValid() );
    Assert( pfmd->pv == NULL );

    if ( fFmHeaderPage )
    {
        Alloc( pfmd->pvWriteBuffer = PvOSMemoryPageAlloc( m_cbFmPageInMemory, NULL ) );
        Alloc( pfmd->pv = new BYTE[ sizeof( FMFILEHDR ) ] );
    }
    else
    {
        Alloc( pfmd->pv = PvOSMemoryPageAlloc( m_cbFmPageInMemory, NULL ) );
        if ( m_fPersisted )
        {
            Alloc( pfmd->rgbitRuntime = PvOSMemoryPageAlloc( m_cbRuntimeBitmapPage, NULL ) );
        }
    }

    pfmd->SetAllocated();

HandleError:

    pfmd->sxwl.ReleaseWriteLatch();

    if ( err < JET_errSuccess )
    {
        FreeFmPage_( pfmd );
    }

    return err;
}

void CFlushMap::FreeFmPage_( FlushMapPageDescriptor* const pfmd )
{
    if ( pfmd == NULL )
    {
        return;
    }
    
    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();
    
    pfmd->sxwl.AcquireWriteLatch();

    if ( fFmHeaderPage )
    {
        OSMemoryPageFree( pfmd->pvWriteBuffer );
        delete[] ( (BYTE*)( pfmd->pv ) );
    }
    else
    {
        OSMemoryPageFree( pfmd->pv );
        OSMemoryPageFree( pfmd->rgbitRuntime );
    }

    pfmd->pvWriteBuffer = NULL;
    pfmd->pv = NULL;
    pfmd->rgbitRuntime = NULL;
    pfmd->ResetAllocated();
    pfmd->sxwl.ReleaseWriteLatch();
}

void CFlushMap::InitializeFmDataPageCapacity_( const CFMPG cfmpgNeeded )
{
    BOOL fFoundValid = fFalse;
    
    m_critFmdGrowCapacity.Enter();
    Assert( cfmpgNeeded <= m_cfmpgAllocated );

    for ( FMPGNO fmpgno = ( m_cfmpgAllocated - 1 ); ( fmpgno >= 0 ) && !fFoundValid; fmpgno-- )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
        pfmd->sxwl.AcquireExclusiveLatch();
        if ( !pfmd->FValid() )
        {
            pfmd->sxwl.UpgradeExclusiveLatchToWriteLatch();
            InitializeFmDataPage_( pfmd );
            pfmd->SetDirty( m_pinst );
            pfmd->SetValid();
            pfmd->sxwl.DowngradeWriteLatchToExclusiveLatch();
        }
        else
        {
            fFoundValid = fTrue;
        }
        pfmd->sxwl.ReleaseExclusiveLatch();
    }

    m_critFmdGrowCapacity.Leave();
}

void CFlushMap::PrereadFlushMap_( const CFMPG cfmpg )
{
    Assert( m_fPersisted );
    Assert( !m_fInitialized );
    Assert( cfmpg <= m_cfmpgAllocated );
    
    CFMPG cfmpgPreread = 0;
    FMPGNO fmpgnoFirst = s_fmpgnoUninit;
    FMPGNO fmpgnoLast = s_fmpgnoUninit;

    for ( FMPGNO fmpgno = 0; fmpgno < cfmpg; fmpgno++ )
    {
        const BOOL fLastPage = ( ( fmpgno + 1 ) == cfmpg );
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
        Assert( pfmd->FAllocated() );
        Assert( !pfmd->FValid() );
        Assert( !pfmd->FDirty() );

        pfmd->sxwl.AcquireWriteLatch();
        const ERR err = ErrAsyncReadFmPage_( pfmd, ( cfmpgPreread == 0 ) ? 0 : qosIOOptimizeCombinable );

        if ( err >= JET_errSuccess )
        {
            cfmpgPreread++;

            if ( cfmpgPreread == 1 )
            {
                fmpgnoFirst = fmpgno;
            }
            fmpgnoLast = fmpgno;
        }
        else
        {
            if ( err == errDiskTilt )
            {
                Assert( cfmpgPreread > 0 );
                fmpgno--;
            }
            else
            {
                Assert( pfmd->FValid() );
                Assert( pfmd->FDirty() );
            }
        }

        if ( ( ( err < JET_errSuccess ) || fLastPage ) && ( cfmpgPreread > 0 ) )
        {
            CallS( m_pfapi->ErrIOIssue() );

            for ( FMPGNO fmpgnoIoPending = fmpgnoFirst; fmpgnoIoPending <= fmpgnoLast; fmpgnoIoPending++ )
            {
                FlushMapPageDescriptor* pfmdIoPending = NULL;
                CallS( ErrGetDescriptorFromFmPgno_( fmpgnoIoPending, &pfmdIoPending ) );
                pfmdIoPending->sxwl.AcquireSharedLatch();
                Assert( pfmdIoPending->FValid() );
                pfmdIoPending->sxwl.ReleaseSharedLatch();
            }

            cfmpgPreread = 0;
            fmpgnoFirst = s_fmpgnoUninit;
            fmpgnoLast = s_fmpgnoUninit;
        }
    }

    Assert( cfmpgPreread == 0 );
}

void CFlushMap::InitializeFmHdr_( FlushMapPageDescriptor* const pfmd )
{
    Assert( pfmd->FIsFmHeader() );
    Assert( pfmd->sxwl.FOwnWriteLatch() );
    memset( pfmd->pvWriteBuffer, 0, s_cbFlushMapPageOnDisk );
    memset( pfmd->pv, 0, sizeof( FMFILEHDR ) );
    new( pfmd->pvWriteBuffer ) FMFILEHDR();
    FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)( pfmd->pvWriteBuffer );

    pfmfilehdr->le_ulFmVersionMajor = ulFMVersionMajorMax;
    pfmfilehdr->le_ulFmVersionUpdateMajor = ulFMVersionUpdateMajorMax;
    pfmfilehdr->le_ulFmVersionUpdateMinor = ulFMVersionUpdateMinorMax;
    LGIGetDateTime( &pfmfilehdr->logtimeCreate );
    UtilMemCpy( &pfmfilehdr->logtimeAttach, &pfmfilehdr->logtimeCreate, sizeof( pfmfilehdr->logtimeAttach ) );
    pfmfilehdr->lGenMinRequired = LGetDbGenMinRequired_();
    pfmfilehdr->filetype = JET_filetypeFlushMap;
}

void CFlushMap::InitializeFmDataPage_( FlushMapPageDescriptor* const pfmd )
{
    Assert( !pfmd->FIsFmHeader() );
    Assert( pfmd->sxwl.FOwnWriteLatch() );
    memset( pfmd->pv, 0, s_cbFlushMapPageOnDisk );
    new( pfmd->pv ) FlushMapDataPageHdr();
    FlushMapDataPageHdr* const pfmpghdr = (FlushMapDataPageHdr*)( pfmd->pv );
    if ( pfmd->rgbitRuntime != NULL )
    {
        memset( pfmd->rgbitRuntime, 0xFF, m_cbRuntimeBitmapPage );
    }

    pfmpghdr->fmpgno = pfmd->fmpgno;
    pfmpghdr->dbtimeMax = dbtimeNil;
    pfmd->dbtimeMax = pfmpghdr->dbtimeMax;
}

void CFlushMap::EnterDbHeaderFlush_( SIGNATURE* const psignDbHdrFlush, SIGNATURE* const psignFlushMapHdrFlush )
{
    AssertSz( m_fGetSetAllowed, "Initiating DB header flush not allowed in EnterDbHeaderFlush_(), m_fGetSetAllowed is fFalse." );
    AssertSz( m_fInitialized, "Initiating DB header flush not allowed in EnterDbHeaderFlush_(), m_fInitialized is fFalse." );
    Assert( !m_fReadOnly );
    Assert( !m_fDumpMode );
    Assert( m_fmdFmHdr.sxwl.FNotOwner() );

    m_fmdFmHdr.sxwl.AcquireExclusiveLatch();
    SIGGetSignature( psignDbHdrFlush );

    if ( m_fPersisted )
    {
        if ( m_fmdFmHdr.FRecentlyAsyncWrittenHeader() && ( ErrUtilFlushFileBuffers( m_pfapi, iofrFmFlush ) >= JET_errSuccess ) )
        {
            m_fmdFmHdr.ResetRecentlyAsyncWrittenHeader();
        }

        UtilMemCpy( psignFlushMapHdrFlush, &( (FMFILEHDR*)( m_fmdFmHdr.pv ) )->signFlushMapHdrFlush, sizeof( SIGNATURE ) );
    }
    else
    {
        SIGResetSignature( psignFlushMapHdrFlush );
    }
}

void CFlushMap::LeaveDbHeaderFlush_()
{
    AssertSz( m_fGetSetAllowed, "Finalizing DB header flush not allowed in LeaveDbHeaderFlush_(), m_fGetSetAllowed is fFalse." );
    AssertSz( m_fInitialized, "Finalizing DB header flush not allowed in LeaveDbHeaderFlush_(), m_fInitialized is fFalse." );
    Assert( !m_fReadOnly );
    Assert( !m_fDumpMode );
    Assert( m_fmdFmHdr.sxwl.FOwnExclusiveLatch() );

    m_fmdFmHdr.sxwl.ReleaseExclusiveLatch();
}

void CFlushMap::PrepareFmPageForRead_( FlushMapPageDescriptor* const pfmd, const BOOL fSync )
{
    pfmd->SetReadInProgress();

    if ( !fSync )
    {
        pfmd->sxwl.ReleaseOwnership( CSXWLatch::iWriteGroup );
    }

    IncrementFlushMapIoCount_();
}

ERR CFlushMap::ErrPrepareFmPageForWrite_( FlushMapPageDescriptor* const pfmd, const BOOL fSync )
{
    ERR err = JET_errSuccess;
    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();

    Assert( pfmd->pv != NULL );
    Assert( ( pfmd->pvWriteBuffer == NULL ) || fFmHeaderPage );
    Assert( ( pfmd->pvWriteBuffer != NULL ) || !fFmHeaderPage );
    Assert( pfmd->sxwl.FOwnWriteLatch() );

    BOOL fDirtyExpected = pfmd->FDirty();

    if ( pfmd->pvWriteBuffer == NULL )
    {
        if ( fSync )
        {
            pfmd->pvWriteBuffer = PvOSMemoryPageAlloc( m_cbFmPageInMemory, NULL );
        }
        else
        {
            Assert( m_pvSectionFlushAsyncWriteBuffer != NULL );
            Assert( m_cbSectionFlushAsyncWriteBufferReserved >= m_cbFmPageInMemory );

            const DWORD ibSectionFlushAsyncWriteBufferNextNew = m_ibSectionFlushAsyncWriteBufferNext + m_cbFmPageInMemory;
            Expected( ibSectionFlushAsyncWriteBufferNextNew <= m_cbSectionFlushAsyncWriteBufferReserved );

            if ( ibSectionFlushAsyncWriteBufferNextNew <= m_cbSectionFlushAsyncWriteBufferReserved )
            {
                if ( ( ibSectionFlushAsyncWriteBufferNextNew > m_cbSectionFlushAsyncWriteBufferCommitted ) &&
                    FOSMemoryPageCommit( (BYTE*)m_pvSectionFlushAsyncWriteBuffer + m_ibSectionFlushAsyncWriteBufferNext, ibSectionFlushAsyncWriteBufferNextNew - m_cbSectionFlushAsyncWriteBufferCommitted ) )
                {
                    m_cbSectionFlushAsyncWriteBufferCommitted = ibSectionFlushAsyncWriteBufferNextNew;
                }

                if ( ibSectionFlushAsyncWriteBufferNextNew <= m_cbSectionFlushAsyncWriteBufferCommitted )
                {
                    pfmd->pvWriteBuffer = (BYTE*)m_pvSectionFlushAsyncWriteBuffer + m_ibSectionFlushAsyncWriteBufferNext;
                    m_ibSectionFlushAsyncWriteBufferNext = ibSectionFlushAsyncWriteBufferNextNew;
                }
            }

            Assert( m_cbSectionFlushAsyncWriteBufferCommitted <= m_cbSectionFlushAsyncWriteBufferReserved );
        }

        Alloc( pfmd->pvWriteBuffer );
    }

    if ( fFmHeaderPage )
    {
        FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)( pfmd->pvWriteBuffer );
        SIGGetSignature( &pfmfilehdr->signFlushMapHdrFlush );
        CopyDbHdrFlushSignatureFromDb_( &pfmfilehdr->signDbHdrFlush );
        AssertSz( fDirtyExpected == pfmd->FDirty(), "Dirty state must not have changed (before SetDirty())." );
        pfmd->SetDirty( m_pinst );
        fDirtyExpected = fTrue;
    }
    else
    {
        UtilMemCpy( pfmd->pvWriteBuffer, pfmd->pv, s_cbFlushMapPageOnDisk );
        
        ( (FlushMapDataPageHdr*)( pfmd->pvWriteBuffer ) )->dbtimeMax = pfmd->dbtimeMax;
    }

    SetPageChecksum( pfmd->pvWriteBuffer, s_cbFlushMapPageOnDisk, fFmHeaderPage ? flushmapHeaderPage : flushmapDataPage, pfmd->fmpgno );

HandleError:

    if ( fSync )
    {
        pfmd->SetSyncWriteInProgress();
    }
    else
    {
        pfmd->SetAsyncWriteInProgress();

        const CMeteredSection::Group groupSectionFlush = m_msSectionFlush.GroupEnter();
        Assert( groupSectionFlush == m_groupSectionFlushWrite );

        pfmd->errAsyncWrite = JET_errSuccess;
    }

    if ( !fSync || fFmHeaderPage )
    {
        AssertSz( fDirtyExpected == pfmd->FDirty(), "Dirty state must not have changed (before ResetDirty())." );
        pfmd->ResetDirty( m_pinst );
        fDirtyExpected = fFalse;
    }

    AssertSz( fDirtyExpected == pfmd->FDirty(), "Dirty state must match." );
    pfmd->sxwl.DowngradeWriteLatchToExclusiveLatch();

    if ( !fSync )
    {
        pfmd->sxwl.ReleaseOwnership( CSXWLatch::iExclusiveGroup );
    }

    IncrementFlushMapIoCount_();

    return err;
}

ERR CFlushMap::ErrReadFmPage_( FlushMapPageDescriptor* const pfmd, const BOOL fSync, const OSFILEQOS qos )
{
    ERR err = JET_errSuccess;
    const IFileAPI::PfnIOComplete pfnIoComplete = fSync ? NULL : OsReadIoComplete_;
    const DWORD_PTR keyIoComplete = fSync ? NULL : (DWORD_PTR)this;

    OnDebug( AssertPreIo_( fFalse, pfmd ) );

    PrepareFmPageForRead_( pfmd, fSync );

    Assert( pfmd->sxwl.FWriteLatched() );
    Assert( ( !fSync && pfmd->sxwl.FNotOwner() ) || ( fSync && pfmd->sxwl.FOwnWriteLatch() ) );

    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();

    TraceContextScope tcScope( iorpFlushMap );

    err = m_pfapi->ErrIORead( *tcScope,
                                OffsetOfFmPgno_( pfmd->fmpgno ),
                                s_cbFlushMapPageOnDisk,
                                (BYTE*)( fFmHeaderPage ? pfmd->pvWriteBuffer : pfmd->pv ),
                                QosSyncDefault( m_pinst ) | qos,
                                pfnIoComplete,
                                keyIoComplete );

    if ( ( err < JET_errSuccess ) || fSync )
    {
        err = ErrReadIoComplete_( err, fSync, pfmd );
        DecrementFlushMapIoCount_();
    }

    return err;
}

ERR CFlushMap::ErrWriteFmPage_( FlushMapPageDescriptor* const pfmd, const BOOL fSync, const OSFILEQOS qos )
{
    ERR err = JET_errSuccess;
    const IFileAPI::PfnIOComplete pfnIoComplete = fSync ? NULL : OsWriteIoComplete_;
    const DWORD_PTR keyIoComplete = fSync ? NULL : (DWORD_PTR)this;
    
    OnDebug( AssertPreIo_( fTrue, pfmd ) );

    err = ErrPrepareFmPageForWrite_( pfmd, fSync );

    Assert( pfmd->sxwl.FExclusiveLatched() );
    Assert( ( !fSync && pfmd->sxwl.FNotOwner() ) || ( fSync && pfmd->sxwl.FOwnExclusiveLatch() ) );

    if ( err >= JET_errSuccess )
    {
        TraceContextScope tcScope( iorpFlushMap );
        err = m_pfapi->ErrIOWrite( *tcScope,
                                    OffsetOfFmPgno_( pfmd->fmpgno ),
                                    s_cbFlushMapPageOnDisk,
                                    (BYTE*)pfmd->pvWriteBuffer,
                                    QosSyncDefault( m_pinst ) | qos,
                                    pfnIoComplete,
                                    keyIoComplete );
    }

    if ( ( err < JET_errSuccess ) || fSync )
    {
        err = ErrWriteIoComplete_( err, fSync, pfmd );
        DecrementFlushMapIoCount_();
    }

    return err;
}

void CFlushMap::OsReadIoComplete_(
    ERR errIo,
    IFileAPI* const pfapi,
    const FullTraceContext& tc,
    const OSFILEQOS grbitQOS,
    const QWORD ibOffset,
    const DWORD cbData,
    const BYTE* const pbData,
    const DWORD_PTR keyIOComplete )
{
    CFlushMap* const pfm = (CFlushMap*)keyIOComplete;

    if ( ( errIo >= JET_errSuccess ) && ( cbData < s_cbFlushMapPageOnDisk ) )
    {
        errIo = ErrERRCheck( JET_errFileIOBeyondEOF );
    }
    
    const FMPGNO fmpgno = FmPgnoOfOffset_( ibOffset );
    FlushMapPageDescriptor* pfmd = NULL;

    AssertSz( !pfm->m_fDumpMode, "Dump mode is not supposed to perform async I/O." );
    CallS( pfm->ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
    (void)pfm->ErrReadIoComplete_( errIo, fFalse, pfmd );
    pfm->DecrementFlushMapIoCount_();
}

void CFlushMap::OsWriteIoComplete_(
    const ERR errIo,
    IFileAPI* const pfapi,
    const FullTraceContext& tc,
    const OSFILEQOS grbitQOS,
    const QWORD ibOffset,
    const DWORD cbData,
    const BYTE* const pbData,
    const DWORD_PTR keyIOComplete )
{
    CFlushMap* const pfm = (CFlushMap*)keyIOComplete;

    Assert( cbData == s_cbFlushMapPageOnDisk );
    
    const FMPGNO fmpgno = FmPgnoOfOffset_( ibOffset );
    FlushMapPageDescriptor* pfmd = NULL;

    AssertSz( !pfm->m_fDumpMode, "Dump mode is not supposed to perform async I/O." );
    CallS( pfm->ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
    (void)pfm->ErrWriteIoComplete_( errIo, fFalse, pfmd );
    pfm->DecrementFlushMapIoCount_();
}

ERR CFlushMap::ErrReadIoComplete_( const ERR errIo, const BOOL fSync, FlushMapPageDescriptor* const pfmd )
{
    const BOOL fIoSuccess = ( errIo >= JET_errSuccess );
    ERR err = errIo;
    const FMPGNO fmpgno = pfmd->fmpgno;
    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );
    BOOL fValid = fFalse;
    OnDebug( AssertOnIoCompletion_( fSync, fFalse, pfmd ) );

    if ( !fSync )
    {
        pfmd->sxwl.ClaimOwnership( CSXWLatch::iWriteGroup );
    }

    if ( fIoSuccess )
    {
        const ERR errValidate = fFmHeaderPage ? ErrValidateFmHdr_( pfmd ) : ErrValidateFmDataPage_( pfmd );

        if ( ( err == JET_errSuccess ) || ( errValidate < JET_errSuccess ) )
        {
            err = errValidate;
        }
    }

    if ( ( err >= JET_errSuccess ) || ( fIoSuccess && m_fDumpMode ) )
    {
        fValid = fTrue;
        if ( fFmHeaderPage )
        {
            UtilMemCpy( pfmd->pv, pfmd->pvWriteBuffer, sizeof( FMFILEHDR ) );
        }
        else
        {
            memset( pfmd->rgbitRuntime, 0, m_cbRuntimeBitmapPage );
            pfmd->dbtimeMax = ( (FlushMapDataPageHdr*)( pfmd->pv ) )->dbtimeMax;
        }
    }
    else if ( errIo != errDiskTilt )
    {
        fValid = fTrue;
        if ( fFmHeaderPage )
        {
            InitializeFmHdr_( pfmd );
        }
        else
        {
            InitializeFmDataPage_( pfmd );
        }

        pfmd->SetDirty( m_pinst );
    }

    pfmd->ResetReadInProgress();

    if ( fValid )
    {
        pfmd->SetValid();
    }

    if ( !fSync )
    {
        pfmd->sxwl.ReleaseWriteLatch();
    }

    OnDebug( AssertPostIo_( errIo, fSync, fFalse, pfmd ) );

    return err;
}

ERR CFlushMap::ErrWriteIoComplete_( const ERR errIo, const BOOL fSync, FlushMapPageDescriptor* const pfmd )
{
    const FMPGNO fmpgno = pfmd->fmpgno;
    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );

    Assert( ( pfmd->pvWriteBuffer != NULL ) || ( ( errIo == JET_errOutOfMemory ) && !fFmHeaderPage ) );
    OnDebug( AssertOnIoCompletion_( fSync, fTrue, pfmd ) );

    if ( ( errIo >= JET_errSuccess ) && ( m_pinst != pinstNil ) )
    {
        PERFOpt( ( fSync ? cFMPagesWrittenSync : cFMPagesWrittenAsync ).Inc( m_pinst->m_iInstance ) );
    }

    if ( !fSync )
    {
        pfmd->sxwl.ClaimOwnership( CSXWLatch::iExclusiveGroup );
    }

    if ( errIo >= JET_errSuccess )
    {
        if ( fFmHeaderPage )
        {
            pfmd->sxwl.UpgradeExclusiveLatchToWriteLatch();
            UtilMemCpy( pfmd->pv, pfmd->pvWriteBuffer, sizeof( FMFILEHDR ) );
            if ( !fSync )
            {
                pfmd->SetRecentlyAsyncWrittenHeader();
            }
            pfmd->sxwl.DowngradeWriteLatchToExclusiveLatch();
        }
    }
    else
    {
        Expected( errIo != errDiskTilt );
        
        if ( !fSync )
        {
            pfmd->errAsyncWrite = errIo;
        }

        pfmd->SetDirty( m_pinst );
    }

    if ( fSync )
    {
        if ( !fFmHeaderPage )
        {
            OSMemoryPageFree( pfmd->pvWriteBuffer );
            pfmd->pvWriteBuffer = NULL;
        }

        pfmd->ResetSyncWriteInProgress();
    }
    else
    {
        if ( !fFmHeaderPage )
        {
            pfmd->pvWriteBuffer = NULL;
        }
        
        pfmd->ResetAsyncWriteInProgress();
        pfmd->sxwl.ReleaseExclusiveLatch();
    }

    Assert( fFmHeaderPage || !pfmd->FRecentlyAsyncWrittenHeader() );

    OnDebug( AssertPostIo_( errIo, fSync, fTrue, pfmd ) );

    if ( !fSync )
    {
        Assert( !m_msSectionFlush.FEmpty() );
        m_msSectionFlush.Leave( m_groupSectionFlushWrite );
    }

    return errIo;
}

ERR CFlushMap::ErrSyncReadFmPage_( FlushMapPageDescriptor* const pfmd )
{
    return ErrReadFmPage_( pfmd, fTrue, 0 );
}

ERR CFlushMap::ErrSyncWriteFmPage_( FlushMapPageDescriptor* const pfmd )
{
    return ErrWriteFmPage_( pfmd, fTrue, 0 );
}

ERR CFlushMap::ErrAsyncReadFmPage_( FlushMapPageDescriptor* const pfmd, const OSFILEQOS qos )
{
    return ErrReadFmPage_( pfmd, fFalse, qos );
}

ERR CFlushMap::ErrAsyncWriteFmPage_( FlushMapPageDescriptor* const pfmd, const OSFILEQOS qos )
{
    return ErrWriteFmPage_( pfmd, fFalse, qos );
}

void CFlushMap::FlushOneSection_( OnDebug2( const BOOL fCleanFlushMap, const BOOL fMapPatching ) )
{
    Assert( m_fPersisted && !m_fReadOnly && !m_fDumpMode );
    Assert( m_sxwlSectionFlush.FOwnWriteLatch() );
    Assert( m_fmpgnoSectionFlushFirst == s_fmpgnoUninit );
    Assert( m_fmpgnoSectionFlushLast == s_fmpgnoUninit );
    Assert( m_ibSectionFlushAsyncWriteBufferNext == 0 );

    Assert( !m_fSectionCheckpointHeaderWrite || ( ( m_fmpgnoSectionFlushNext == s_fmpgnoHdr ) && !fCleanFlushMap ) );

    ERR err = JET_errSuccess;
    FMPGNO fmpgnoSectionFlushFirst = s_fmpgnoUninit;
    FMPGNO fmpgnoSectionFlushLast = s_fmpgnoUninit;
    FMPGNO fmpgnoUsedMax = 0;
    CFMPG cfmpgAsyncWrite = 0;
    CFMPG cfmpgWriteMax = 0;
    CFMPG cfmpgWriteGapMax = 0;

    Assert( m_msSectionFlush.FEmpty() );
    m_groupSectionFlushWrite = m_msSectionFlush.GroupEnter();

    Assert( m_groupSectionFlushWrite >= 0 );

    if ( m_fmpgnoSectionFlushNext == s_fmpgnoHdr )
    {
        m_fmdFmHdr.sxwl.AcquireWriteLatch();
        Call( ErrAsyncWriteFmPage_( &m_fmdFmHdr, 0 ) );
        cfmpgAsyncWrite++;
        fmpgnoSectionFlushFirst = s_fmpgnoHdr;
        fmpgnoSectionFlushLast = s_fmpgnoHdr;
        goto HandleError;
    }

    if ( m_fmpgnoSectionFlushNext == s_fmpgnoUninit )
    {
        const LONG lGenMinDbConsistent = LGetDbGenMinConsistent_();
        Expected( ( lGenMinDbConsistent > 0 ) || !m_fRecoverable || fCleanFlushMap );

        m_fmdFmHdr.sxwl.AcquireWriteLatch();
        FMFILEHDR* const pfmfilehdrInMemory = (FMFILEHDR*)( m_fmdFmHdr.pvWriteBuffer );

        const LONG lGenMinRequiredTargetNew = LFunctionalMax( lGenMinDbConsistent, pfmfilehdrInMemory->lGenMinRequired );
        Assert( ( lGenMinRequiredTargetNew > 0 ) || !m_fRecoverable || fCleanFlushMap );

        Assert( ( lGenMinRequiredTargetNew >= pfmfilehdrInMemory->lGenMinRequired ) || fMapPatching );
        Assert( ( lGenMinRequiredTargetNew >= m_lgenTargetedMax ) || fMapPatching );
        m_lgenTargetedMax = LFunctionalMax( m_lgenTargetedMax, lGenMinRequiredTargetNew );
        pfmfilehdrInMemory->lGenMinRequiredTarget = lGenMinRequiredTargetNew;
        LGIGetDateTime( &pfmfilehdrInMemory->logtimeFlushLastStarted );
        m_fmdFmHdr.SetDirty( m_pinst );
        Call( ErrAsyncWriteFmPage_( &m_fmdFmHdr, 0 ) );
        cfmpgAsyncWrite++;
        fmpgnoSectionFlushFirst = s_fmpgnoHdr;
        fmpgnoSectionFlushLast = s_fmpgnoHdr;
        goto HandleError;
    }

    fmpgnoUsedMax = m_cfmpgAllocated - 1;

    for ( fmpgnoSectionFlushFirst = m_fmpgnoSectionFlushNext; fmpgnoSectionFlushFirst <= fmpgnoUsedMax; fmpgnoSectionFlushFirst++ )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgnoSectionFlushFirst, &pfmd ) );
        Assert( pfmd->FAllocated() );

        if ( !pfmd->FDirty() )
        {
            continue;
        }

        if ( pfmd->FValid() )
        {
            break;
        }
    }

    if ( fmpgnoSectionFlushFirst > fmpgnoUsedMax )
    {
        if ( m_fmdFmHdr.FRecentlyAsyncWrittenHeader() )
        {
            Call( ErrUtilFlushFileBuffers( m_pfapi, iofrFmFlush ) );
            m_fmdFmHdr.ResetRecentlyAsyncWrittenHeader();
        }

        m_fmdFmHdr.sxwl.AcquireWriteLatch();

        FMFILEHDR* const pfmfilehdrInMemory = (FMFILEHDR*)( m_fmdFmHdr.pvWriteBuffer );

        Assert( ( pfmfilehdrInMemory->lGenMinRequiredTarget >= pfmfilehdrInMemory->lGenMinRequired ) || fMapPatching );
        pfmfilehdrInMemory->lGenMinRequired = pfmfilehdrInMemory->lGenMinRequiredTarget;
        Assert( ( pfmfilehdrInMemory->lGenMinRequired > 0 ) || !m_fRecoverable || fCleanFlushMap );
        pfmfilehdrInMemory->lGenMinRequiredTarget = 0;
        LGIGetDateTime( &pfmfilehdrInMemory->logtimeFlushLastCompleted );
        m_fmdFmHdr.SetDirty( m_pinst );

        Call( ErrAsyncWriteFmPage_( &m_fmdFmHdr, 0 ) );
        cfmpgAsyncWrite++;
        fmpgnoSectionFlushFirst = s_fmpgnoHdr;
        fmpgnoSectionFlushLast = s_fmpgnoHdr;

        m_fSectionCheckpointHeaderWrite = fTrue;

        goto HandleError;
    }

    cfmpgWriteMax = (CFMPG)UlFunctionalMax( ( (DWORD)UlParam( JET_paramMaxCoalesceWriteSize ) / s_cbFlushMapPageOnDisk ), 1 );
    cfmpgWriteGapMax = (CFMPG)( ( (DWORD)UlParam( JET_paramMaxCoalesceWriteGapSize ) / s_cbFlushMapPageOnDisk ) );
    fmpgnoSectionFlushLast = fmpgnoSectionFlushFirst - 1;
    
    for ( FMPGNO fmpgnoSectionFlushCheck = fmpgnoSectionFlushFirst;
            ( fmpgnoSectionFlushCheck <= fmpgnoUsedMax ) && ( ( fmpgnoSectionFlushCheck - fmpgnoSectionFlushFirst + 1 ) <= cfmpgWriteMax );
            fmpgnoSectionFlushCheck++ )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgnoSectionFlushCheck, &pfmd ) );

        Assert( ( pfmd->FDirty() && pfmd->FValid() ) || ( fmpgnoSectionFlushCheck > fmpgnoSectionFlushFirst ) );

        if ( !pfmd->FValid() )
        {
            break;
        }

        if ( pfmd->FDirty() )
        {
            FMPGNO fmpgnoSectionFlush;
            for ( fmpgnoSectionFlush = fmpgnoSectionFlushLast + 1; fmpgnoSectionFlush <= fmpgnoSectionFlushCheck; fmpgnoSectionFlush++ )
            {
                FlushMapPageDescriptor* pfmdWrite = NULL;
                CallS( ErrGetDescriptorFromFmPgno_( fmpgnoSectionFlush, &pfmdWrite ) );
                Assert( pfmdWrite->FValid() );
                pfmdWrite->sxwl.AcquireWriteLatch();
                err = ErrAsyncWriteFmPage_( pfmdWrite, ( cfmpgAsyncWrite == 0 ) ? 0 : qosIOOptimizeCombinable );
                if ( err < JET_errSuccess )
                {
                    break;
                }

                fmpgnoSectionFlushLast = fmpgnoSectionFlush;
                cfmpgAsyncWrite++;
            }

            if ( fmpgnoSectionFlush <= fmpgnoSectionFlushCheck )
            {
                break;
            }
        }
        else
        {
            if ( ( fmpgnoSectionFlushCheck - fmpgnoSectionFlushLast ) > cfmpgWriteGapMax )
            {
                break;
            }
        }
    }

HandleError:

    m_errSectionFlushWriteLastError = err;

    if ( cfmpgAsyncWrite > 0 )
    {
        Assert( fmpgnoSectionFlushFirst != s_fmpgnoUninit );
        Assert( fmpgnoSectionFlushLast != s_fmpgnoUninit );
        Assert( fmpgnoSectionFlushLast >= fmpgnoSectionFlushFirst );
        Assert( cfmpgAsyncWrite == ( fmpgnoSectionFlushLast - fmpgnoSectionFlushFirst + 1 ) );

        Assert( ( fmpgnoSectionFlushFirst != s_fmpgnoHdr ) || ( fmpgnoSectionFlushLast == s_fmpgnoHdr ) );
        m_fmpgnoSectionFlushFirst = fmpgnoSectionFlushFirst;
        m_fmpgnoSectionFlushLast = fmpgnoSectionFlushLast;

        m_msSectionFlush.Partition( SectionFlushCompletion_, (DWORD_PTR)this );
        m_sxwlSectionFlush.ReleaseOwnership( CSXWLatch::iWriteGroup );
        m_msSectionFlush.Leave( m_groupSectionFlushWrite );

        CallS( m_pfapi->ErrIOIssue() );
    }
    else
    {
        m_ibSectionFlushAsyncWriteBufferNext = 0;
        m_msSectionFlush.Leave( m_groupSectionFlushWrite );
        Assert( m_msSectionFlush.FEmpty() );
        m_sxwlSectionFlush.ReleaseWriteLatch();
    }
}


ERR CFlushMap::ErrFlushAllSections_( OnDebug2( const BOOL fCleanFlushMap, const BOOL fMapPatching ) )
{
    Assert( !m_fReadOnly && !m_fDumpMode );
    Assert( m_sxwlSectionFlush.FOwnWriteLatch() );

    ERR err = JET_errSuccess;

    if ( m_fmpgnoSectionFlushNext != s_fmpgnoUninit )
    {
        m_fmpgnoSectionFlushNext = s_fmpgnoHdr;
        m_fSectionCheckpointHeaderWrite = fFalse;
    }
    else
    {
        Assert( !m_fSectionCheckpointHeaderWrite );
    }

    Expected( fCleanFlushMap || fMapPatching );
    Expected( !( fCleanFlushMap && fMapPatching ) );
    do
    {
        FlushOneSection_( OnDebug2( fCleanFlushMap, fMapPatching ) );
        m_sxwlSectionFlush.AcquireWriteLatch();
    }
    while ( ( ( err = m_errSectionFlushWriteLastError ) >= JET_errSuccess ) && ( m_fmpgnoSectionFlushNext != s_fmpgnoUninit ) );

    Assert( m_sxwlSectionFlush.FOwnWriteLatch() );

    return err;
}

void CFlushMap::CompleteOneSectionFlush_()
{
    Assert( m_fPersisted && !m_fReadOnly );
    Assert( m_sxwlSectionFlush.FWriteLatched() );
    Assert( m_sxwlSectionFlush.FNotOwner() );
    Assert( m_fmpgnoSectionFlushFirst != s_fmpgnoUninit );
    Assert( m_fmpgnoSectionFlushLast != s_fmpgnoUninit );
    Assert( m_msSectionFlush.FEmpty() );
    Assert( !m_fSectionCheckpointHeaderWrite || ( ( m_fmpgnoSectionFlushFirst == m_fmpgnoSectionFlushLast ) && ( m_fmpgnoSectionFlushFirst == s_fmpgnoHdr ) ) );
    Assert( m_fmpgnoSectionFlushFirst <= m_fmpgnoSectionFlushLast );
    Assert( ( m_fmpgnoSectionFlushFirst == m_fmpgnoSectionFlushLast ) || ( ( m_fmpgnoSectionFlushFirst != s_fmpgnoHdr ) && ( m_fmpgnoSectionFlushLast != s_fmpgnoHdr ) ) );

    m_sxwlSectionFlush.ClaimOwnership( CSXWLatch::iWriteGroup );

    FMPGNO fmpgno;
    for ( fmpgno = m_fmpgnoSectionFlushFirst; fmpgno <= m_fmpgnoSectionFlushLast;)
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );

        Assert( !pfmd->FAsyncWriteInProgress() );

        if ( pfmd->errAsyncWrite < JET_errSuccess )
        {
            Assert( pfmd->FDirty() );
            if ( m_errSectionFlushWriteLastError >= JET_errSuccess )
            {
                m_errSectionFlushWriteLastError = pfmd->errAsyncWrite;
            }
            break;
        }

        if ( fmpgno == s_fmpgnoHdr )
        {
            Assert( m_fmpgnoSectionFlushFirst == m_fmpgnoSectionFlushLast );
            fmpgno = 0;
        }
        else
        {
            fmpgno++;
        }
    }

    const BOOL fFailureSeen = ( m_errSectionFlushWriteLastError < JET_errSuccess );
    const BOOL fSuccessfullyCompletedCheckpoint = m_fSectionCheckpointHeaderWrite && !fFailureSeen;
    m_ibSectionFlushAsyncWriteBufferNext = 0;
    m_fmpgnoSectionFlushFirst = s_fmpgnoUninit;
    m_fmpgnoSectionFlushLast = s_fmpgnoUninit;
    m_fmpgnoSectionFlushNext = fSuccessfullyCompletedCheckpoint ? s_fmpgnoUninit : fmpgno;
    m_groupSectionFlushWrite = CMeteredSection::groupInvalidNil;
    if ( fSuccessfullyCompletedCheckpoint )
    {
        m_cbLogGeneratedAtLastFlushProgress = ullMax;
        m_fSectionCheckpointHeaderWrite = fFalse;
    }

    m_sxwlSectionFlush.ReleaseWriteLatch();
}

void CFlushMap::SectionFlushCompletion_( const DWORD_PTR dwCompletionKey )
{
    CFlushMap* const pfm = (CFlushMap*)dwCompletionKey;
    pfm->CompleteOneSectionFlush_();
}

void CFlushMap::AllowFlushMapIo_()
{
    OnDebug( AssertFlushMapIoDisallowed_() );
    const LONG cInFlightIo = AtomicExchange( &m_cInFlightIo, 0 );
    Assert( cInFlightIo == lMin );
    OnDebug( AssertFlushMapIoAllowed_() );
}

void CFlushMap::DisallowFlushMapIo_()
{
    OnDebug( AssertFlushMapIoAllowedAndNotInFlight_() );
    const LONG cInFlightIo = AtomicExchange( &m_cInFlightIo, lMin );
    Assert( cInFlightIo == 0 );
    OnDebug( AssertFlushMapIoDisallowed_() );
}

void CFlushMap::WaitForFlushMapIoQuiesce_()
{
    while ( AtomicRead( &m_cInFlightIo ) != 0 )
    {
        OnDebug( AssertFlushMapIoAllowed_() );
        UtilSleep( 1 );
    }
}

void CFlushMap::IncrementFlushMapIoCount_()
{
    OnDebug( AssertFlushMapIoAllowed_() );
    const LONG cInFlightIo = AtomicIncrement( &m_cInFlightIo );
    Assert( cInFlightIo > 0 );
    OnDebug( AssertFlushMapIoAllowedAndInFlight_() );
}

void CFlushMap::DecrementFlushMapIoCount_()
{
    OnDebug( AssertFlushMapIoAllowedAndInFlight_() );
    const LONG cInFlightIo = AtomicDecrement( &m_cInFlightIo );
    Assert( cInFlightIo >= 0 );
}

#ifdef DEBUG

void CFlushMap::AssertFlushMapIoAllowed_()
{
    Assert( AtomicRead( &m_cInFlightIo ) >= 0 );
}

void CFlushMap::AssertFlushMapIoDisallowed_()
{
    Assert( AtomicRead( &m_cInFlightIo ) == lMin );
}

void CFlushMap::AssertFlushMapIoAllowedAndInFlight_()
{
    Assert( AtomicRead( &m_cInFlightIo ) > 0 );
}

void CFlushMap::AssertFlushMapIoAllowedAndNotInFlight_()
{
    Assert( AtomicRead( &m_cInFlightIo ) == 0 );
}

void CFlushMap::AssertPreIo_( const BOOL fWrite, FlushMapPageDescriptor* const pfmd )
{
    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();
    const void* const pv = fFmHeaderPage ? pfmd->pvWriteBuffer : pfmd->pv;
    Assert( m_fPersisted );
    Assert( pfmd->sxwl.FOwnWriteLatch() );
    Assert( pfmd->FAllocated() );
    Assert( !pfmd->FReadInProgress() );
    Assert( !pfmd->FWriteInProgress() );
    Assert( ( fFmHeaderPage && ( pfmd->rgbitRuntime == NULL ) ) || ( !fFmHeaderPage && ( pfmd->rgbitRuntime != NULL ) ) );
    AssertFlushMapIoAllowed_();

    if ( fWrite )
    {
        Assert( !m_fReadOnly );
        Assert( CPAGE::FPageIsInitialized( pv, s_cbFlushMapPageOnDisk ) );
        Assert( pfmd->FValid() );
    }
    else
    {
        Assert( !CPAGE::FPageIsInitialized( pv, s_cbFlushMapPageOnDisk ) );
        Assert( !pfmd->FValid() );
        Assert( !pfmd->FDirty() );
        Assert( pfmd->errAsyncWrite == JET_errSuccess );
    }
}

void CFlushMap::AssertOnIoCompletion_( const BOOL fSync, const BOOL fWrite, FlushMapPageDescriptor* const pfmd )
{
    AssertFlushMapIoAllowedAndInFlight_();
    
    if ( fWrite )
    {
        if ( pfmd->pvWriteBuffer != NULL )
        {
            Assert( ErrChecksumFmPage_( pfmd->pvWriteBuffer, pfmd->fmpgno ) == JET_errSuccess );
        }
        
        Assert( !pfmd->FReadInProgress() );

        if ( fSync )
        {
            Assert( pfmd->FSyncWriteInProgress() );
            Assert( !pfmd->FAsyncWriteInProgress() );
        }
        else
        {
            Assert( !pfmd->FSyncWriteInProgress() );
            Assert( pfmd->FAsyncWriteInProgress() );
        }

        Assert( pfmd->FValid() );
    }
    else
    {
        Assert( pfmd->sxwl.FWriteLatched() );
        Assert( pfmd->FReadInProgress() );
        Assert( !pfmd->FWriteInProgress() );
        Assert( !pfmd->FValid() );
        Assert( !pfmd->FDirty() );
        Assert( pfmd->errAsyncWrite == JET_errSuccess );
    }
}

void CFlushMap::AssertPostIo_( const ERR err, const BOOL fSync, const BOOL fWrite, FlushMapPageDescriptor* const pfmd )
{
    AssertFlushMapIoAllowedAndInFlight_();
    
    if ( fWrite || ( err != errDiskTilt ) )
    {
        Assert( pfmd->FValid() );
    }

    if ( fSync )
    {
        if ( fWrite )
        {
            Assert( pfmd->sxwl.FOwnExclusiveLatch() );
        }
        else
        {
            Assert( pfmd->sxwl.FOwnWriteLatch() );
        }
    }
    else
    {
        Assert( pfmd->sxwl.FNotOwner() );
    }
}

#endif

ERR CFlushMap::ErrChecksumFmPage_( FlushMapPageDescriptor* const pfmd )
{
    Assert( pfmd->sxwl.FOwnWriteLatch() );
    const BOOL fFmHeaderPage = pfmd->FIsFmHeader();
    return ErrChecksumFmPage_( fFmHeaderPage ? pfmd->pvWriteBuffer : pfmd->pv, pfmd->fmpgno );
}

ERR CFlushMap::ErrValidateFmHdr_( FlushMapPageDescriptor* const pfmd )
{
    Assert( pfmd->FIsFmHeader() );
    ERR err = JET_errSuccess;
    const FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)( pfmd->pvWriteBuffer );
    WCHAR wszAdditionalInfo[ 512 ] = { L'\0' };
    SIGNATURE signDbHdrFlushFromDb, signFlushMapHdrFlushFromDb;

    Call( ErrChecksumFmPage_( pfmd ) );

    if ( pfmfilehdr->filetype != JET_filetypeFlushMap )
    {
        OSStrCbFormatW( wszAdditionalInfo, sizeof(wszAdditionalInfo),
            L"[FileType:%lu]",
            (ULONG)pfmfilehdr->filetype );
        Error( ErrERRCheck( JET_errFileInvalidType ) );
    }

    if ( ( pfmfilehdr->le_ulFmVersionMajor != ulFMVersionMajorMax ) ||
            ( pfmfilehdr->le_ulFmVersionUpdateMajor > ulFMVersionUpdateMajorMax ) )
    {
        OSStrCbFormatW( wszAdditionalInfo, sizeof(wszAdditionalInfo),
            L"[FmVersionSupported:%lu.%lu.%lu] [FmVersionActual:%lu.%lu.%lu]",
            ulFMVersionMajorMax,
            ulFMVersionUpdateMajorMax,
            ulFMVersionUpdateMinorMax,
            (ULONG)pfmfilehdr->le_ulFmVersionMajor,
            (ULONG)pfmfilehdr->le_ulFmVersionUpdateMajor,
            (ULONG)pfmfilehdr->le_ulFmVersionUpdateMinor );
        Error( ErrERRCheck( JET_errFlushMapVersionUnsupported ) );
    }

    if ( !m_fDumpMode )
    {
        GetFlushSignaturesFromDb_( pfmfilehdr, &signDbHdrFlushFromDb, &signFlushMapHdrFlushFromDb );
        if ( !FCheckForDbFmConsistency_( &signDbHdrFlushFromDb, &signFlushMapHdrFlushFromDb, &pfmfilehdr->signDbHdrFlush, &pfmfilehdr->signFlushMapHdrFlush ) )
        {
            CHAR szDbHdrFlushFromDbSig[ 128 ];
            CHAR szFmHdrFlushFromDbSig[ 128 ];
            CHAR szDbHdrFlushFromFmSig[ 128 ];
            CHAR szFmHdrFlushFromFmSig[ 128 ];
            SigToSz( &signDbHdrFlushFromDb, szDbHdrFlushFromDbSig, sizeof(szDbHdrFlushFromDbSig) );
            SigToSz( &signFlushMapHdrFlushFromDb, szFmHdrFlushFromDbSig, sizeof(szFmHdrFlushFromDbSig) );
            SigToSz( &pfmfilehdr->signDbHdrFlush, szDbHdrFlushFromFmSig, sizeof(szDbHdrFlushFromFmSig) );
            SigToSz( &pfmfilehdr->signFlushMapHdrFlush, szFmHdrFlushFromFmSig, sizeof(szFmHdrFlushFromFmSig) );

            OSStrCbFormatW( wszAdditionalInfo, sizeof(wszAdditionalInfo),
                L"[SignDbHdrFromDb:%hs] [SignFmHdrFromDb:%hs] [SignDbHdrFromFm:%hs] [SignFmHdrFromFm:%hs]",
                szDbHdrFlushFromDbSig,
                szFmHdrFlushFromDbSig,
                szDbHdrFlushFromFmSig,
                szFmHdrFlushFromFmSig );
            Error( ErrERRCheck( JET_errFlushMapDatabaseMismatch ) );
        }
    }

    if ( !m_fDumpMode )
    {
        if ( !( pfmfilehdr->FClean() ||
            ( m_fRecoverable &&
                ( LGetDbGenMinRequired_() > 0 ) &&
                ( pfmfilehdr->lGenMinRequired > 0 ) &&
                ( LGetDbGenMinRequired_() <= pfmfilehdr->lGenMinRequired ) ) )
            )
        {
            OSStrCbFormatW( wszAdditionalInfo, sizeof(wszAdditionalInfo),
                L"[DbState:%lu] [DbMinReq:%d] [FmState:%d] [FmMinReq:%d]",
                UlGetDbState_(),
                LGetDbGenMinRequired_(),
                (INT)pfmfilehdr->FClean(),
                (LONG)pfmfilehdr->lGenMinRequired );
            Error( ErrERRCheck( JET_errFlushMapUnrecoverable ) );
        }
    }

HandleError:

    if ( err < JET_errSuccess )
    {
        LogEventFmHdrValidationFailed_( err, wszAdditionalInfo );
    }

    return err;
}

ERR CFlushMap::ErrValidateFmDataPage_( FlushMapPageDescriptor* const pfmd )
{
    Assert( !pfmd->FIsFmHeader() );
    ERR err = JET_errSuccess;

    Call( ErrChecksumFmPage_( pfmd ) );

HandleError:

    if ( err < JET_errSuccess && ( err != JET_errPageNotInitialized ) )
    {
        LogEventFmDataPageValidationFailed_( err, pfmd->fmpgno );
    }

    return err;
}

ERR CFlushMap::ErrSetRangePgnoFlushType_( const PGNO pgnoFirst, const CPG cpg, const CPAGE::PageFlushType pgft, const DBTIME dbtime, const BOOL fWait )
{
    ERR err = JET_errSuccess;
    FlushMapPageDescriptor* pfmd = NULL;
    FMPGNO fmpgnoFirst = s_fmpgnoUninit;
    FMPGNO fmpgnoLast = s_fmpgnoUninit;

    AssertSz( m_fGetSetAllowed && !m_fDumpMode, "Setting flush type not allowed in CFlushMap::ErrSetRangePgnoFlushType_()." );
    AssertSz( !m_fCleanForTerm, "Setting flush type not allowed with clean map." );
    Expected( cpg > 0 );

    for ( PGNO pgno = pgnoFirst; pgno < ( pgnoFirst + cpg ); pgno++ )
    {
        const FlushMapPageDescriptor* const pfmdT = pfmd;
        Call( ErrGetDescriptorFromPgno_( pgno, &pfmd ) );

        if ( pfmd != pfmdT )
        {
            if ( !pfmd->FValid() )
            {
                Error( ErrERRCheck( JET_errInvalidOperation ) );
            }
        }

        Assert( pfmd->FValid() );
        Assert( pfmd->sxwl.FNotOwner() );
        pfmd->sxwl.AcquireSharedLatch();

        const BOOL fValidDbTime = ( ( dbtime != dbtimeNil ) && ( dbtime != dbtimeInvalid ) && ( dbtime != dbtimeShrunk ) && ( dbtime > 0 ) && ( dbtime != dbtimeRevert ) );
        Expected( fValidDbTime ||
            ( dbtime == dbtimeNil ) ||
            ( dbtime == 0 ) ||
            ( dbtime == dbtimeShrunk ) ||
            ( dbtime == dbtimeRevert ) );

#ifndef ENABLE_JET_UNIT_TEST
        Expected( ( cpg == 1 ) || ( pgft == CPAGE::pgftUnknown ) );

        Assert( ( cpg == 1 ) || ( dbtime == dbtimeNil ) );

        Assert( fValidDbTime || ( pgft == CPAGE::pgftUnknown ) );
#endif

        if ( fValidDbTime )
        {
            AtomicExchangeMax( &pfmd->dbtimeMax, dbtime );
        }

        SetFlushType_( pfmd, pgno, pgft );

        SetFlushTypeRuntimeState_( pfmd, pgno, fTrue );

        pfmd->SetDirty( m_pinst );
        pfmd->sxwl.ReleaseSharedLatch();

        if ( pgno == pgnoFirst )
        {
            fmpgnoFirst = pfmd->fmpgno;
        }
        fmpgnoLast = pfmd->fmpgno;
    }

    if ( fWait && m_fPersisted )
    {
        Assert( ( fmpgnoFirst >= 0 ) && ( fmpgnoFirst <= fmpgnoLast ) );

        for ( FMPGNO fmpgno = fmpgnoFirst; fmpgno <= fmpgnoLast; fmpgno++ )
        {
            Call( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
            Assert( pfmd->FValid() );

            pfmd->sxwl.AcquireWriteLatch();
            err = ErrSyncWriteFmPage_( pfmd );
            pfmd->sxwl.ReleaseExclusiveLatch();
            if ( err < JET_errSuccess )
            {
                goto HandleError;
            }
        }
    }

HandleError:

    return err;
}

INLINE USHORT CFlushMap::IbitGetBitInPage_( const PGNO pgno, const size_t cbHeader, const DWORD cbitPerState )
{
    Assert( ( cbitPerState > 0 ) && ( cbitPerState <= 8 ) );
    return (USHORT)( 8 * cbHeader + ( pgno % m_cDbPagesPerFlushMapPage ) * cbitPerState );
}

INLINE INT CFlushMap::IGetStateFromBitmap_(
    BYTE* const pbBitmap, const size_t cbBitmap,
    const PGNO pgno,
    const size_t cbHeader, const DWORD cbitPerState, const BYTE mask )
{
    const USHORT ibitInBitmap = IbitGetBitInPage_( pgno, cbHeader, cbitPerState );
    const USHORT ibInBitmap = (USHORT)( ibitInBitmap / 8 );
    Assert( ibInBitmap < cbBitmap );

    const BYTE ibitInByte = (BYTE)( ibitInBitmap % 8 );
    Assert( ibitInByte <= ( 8 - cbitPerState ) );
    Assert( ( ibitInByte % cbitPerState ) == 0 );

    volatile BYTE* const pb = pbBitmap + ibInBitmap;
    const BYTE byte = *pb;

    const INT state = (INT)( ( byte & ( mask >> ibitInByte ) ) >> ( 8 - cbitPerState - ibitInByte ) );
    Assert( state >= 0 );
    Assert( state < ( 0x01 << cbitPerState ) );

    return state;
}

INLINE void CFlushMap::SetStateOnBitmap_(
    BYTE* const pbBitmap, const size_t cbBitmap,
    const PGNO pgno, const INT state,
    const size_t cbHeader, const DWORD cbitPerState, const BYTE mask )
{
    Assert( state >= 0 );
    Assert( state < ( 0x01 << cbitPerState ) );

    const USHORT ibitInBitmap = IbitGetBitInPage_( pgno, cbHeader, cbitPerState );
    Assert( ibitInBitmap <= ( ( 8 * cbBitmap ) - cbitPerState ) );

    const BYTE cbitInDword = 8 * sizeof( DWORD );
    const USHORT idwInBitmap = (USHORT)( ibitInBitmap / cbitInDword );
    Assert( idwInBitmap < ( cbBitmap / sizeof( DWORD ) ) );

    const USHORT ibInDword = (USHORT)( ( ibitInBitmap % cbitInDword ) / 8 );
    Assert( ibInDword < sizeof( DWORD ) );

    const BYTE ibitInByte = (BYTE)( ibitInBitmap % 8 );
    Assert( ibitInByte <= ( 8 - cbitPerState ) );
    Assert( ( ibitInByte % cbitPerState ) == 0 );

    volatile DWORD* const pdw = (DWORD*)pbBitmap + idwInBitmap;
    OSSYNC_FOREVER
    {
        const DWORD dwInitial = *pdw;
        DWORD dwTarget = dwInitial;
        BYTE* const pb = ( (BYTE*)&dwTarget ) + ibInDword;
        *pb = ( *pb & ~( mask >> ibitInByte ) ) | ( (BYTE)state << ( 8 - cbitPerState - ibitInByte ) );
        const DWORD dwFinal = AtomicCompareExchange( (LONG*)pdw, (LONG)dwInitial, (LONG)dwTarget );

        if ( dwFinal == dwInitial )
        {
            break;
        }
    }
}

CPAGE::PageFlushType CFlushMap::PgftGetFlushType_( FlushMapPageDescriptor* const pfmd, const PGNO pgno )
{
    const INT state = IGetStateFromBitmap_( (BYTE*)pfmd->pv, s_cbFlushMapPageOnDisk, pgno, sizeof( FlushMapDataPageHdr ), s_cbitFlushType, s_mskFlushType );
    Assert( state >= 0 );
    Assert( state < (INT)CPAGE::pgftMax );

    return (CPAGE::PageFlushType)state;
}

void CFlushMap::SetFlushType_( FlushMapPageDescriptor* const pfmd, const PGNO pgno, const CPAGE::PageFlushType pgft )
{
    const INT state = (INT)pgft;
    Assert( state >= 0 );
    Assert( state < (INT)CPAGE::pgftMax );

    SetStateOnBitmap_( (BYTE*)pfmd->pv, s_cbFlushMapPageOnDisk, pgno, state, sizeof( FlushMapDataPageHdr ), s_cbitFlushType, s_mskFlushType );
}

BOOL CFlushMap::FGetFlushTypeRuntime_( FlushMapPageDescriptor* const pfmd, const PGNO pgno )
{
    if ( pfmd->rgbitRuntime == NULL )
    {
        return fTrue;
    }

    const INT state = IGetStateFromBitmap_( (BYTE*)pfmd->rgbitRuntime, m_cbRuntimeBitmapPage, pgno, 0, 1, 0x80 );
    Assert( ( state == 0 ) || ( state == 1 ) );

    return ( state == 1 );
}

void CFlushMap::SetFlushTypeRuntimeState_( FlushMapPageDescriptor* const pfmd, const PGNO pgno, const BOOL fRuntime )
{
    if ( pfmd->rgbitRuntime == NULL )
    {
        return;
    }

    const INT state = fRuntime ? 1 : 0;

    SetStateOnBitmap_( (BYTE*)pfmd->rgbitRuntime, m_cbRuntimeBitmapPage, pgno, state, 0, 1, 0x80 );
}

void CFlushMap::LogEventFmFileAttachFailed_( const WCHAR* const wszFmFilePath, const ERR err )
{
    WCHAR wszError[ 32 ];
    OSStrCbFormatW( wszError, sizeof( wszError ), L"%ld", err );

    const WCHAR* rgpwsz[] = { wszFmFilePath, wszError };

    UtilReportEvent(
        eventError,
        GENERAL_CATEGORY,
        FLUSHMAP_FILE_ATTACH_FAILED_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

void CFlushMap::LogEventFmFileDeleted_( const WCHAR* const wszFmFilePath, const WCHAR* const wszReason )
{
    const WCHAR* rgpwsz[] = { wszFmFilePath, wszReason };

    UtilReportEvent(
        eventWarning,
        GENERAL_CATEGORY,
        FLUSHMAP_FILE_DELETED_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

void CFlushMap::LogEventFmFileCreated_( const WCHAR* const wszFmFilePath )
{
    const WCHAR* rgpwsz[] = { wszFmFilePath };

    UtilReportEvent(
        eventInformation,
        GENERAL_CATEGORY,
        FLUSHMAP_FILE_CREATED_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

void CFlushMap::LogEventFmHdrValidationFailed_( const ERR err, const WCHAR* const wszAdditionalInfo )
{
    WCHAR wszError[ 32 ];
    OSStrCbFormatW( wszError, sizeof( wszError ), L"%ld", err );
    WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ];
    if ( m_pfapi->ErrPath( wszFmFilePath ) < JET_errSuccess )
    {
        wszFmFilePath[ 0 ] = L'\0';
    }

    const WCHAR* rgpwsz[] = { wszError, wszFmFilePath, wszAdditionalInfo };

    UtilReportEvent(
        eventWarning,
        GENERAL_CATEGORY,
        FLUSHMAP_HEADER_ERROR_VALIDATE_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

void CFlushMap::LogEventFmDataPageValidationFailed_( const ERR err, const FMPGNO fmpgno )
{
    WCHAR wszError[ 32 ];
    OSStrCbFormatW( wszError, sizeof( wszError ), L"%ld", err );
    WCHAR wszFmPgno[ 64 ];
    OSStrCbFormatW( wszFmPgno, sizeof( wszFmPgno ), L"%ld (0x%08lx)", fmpgno, fmpgno );
    WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ];
    if ( m_pfapi->ErrPath( wszFmFilePath ) < JET_errSuccess )
    {
        wszFmFilePath[ 0 ] = L'\0';
    }

    const WCHAR* rgpwsz[] = { wszError, wszFmPgno, wszFmFilePath };

    UtilReportEvent(
        eventWarning,
        GENERAL_CATEGORY,
        FLUSHMAP_DATAPAGE_ERROR_VALIDATE_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

void CFlushMap::LogEventFmInconsistentDbTime_( const FMPGNO fmpgno, const DBTIME dbtimeMaxFm, const PGNO pgno, const DBTIME dbtimeMaxDb )
{
    WCHAR wszFmPgno[ 64 ];
    OSStrCbFormatW( wszFmPgno, sizeof( wszFmPgno ), L"%ld (0x%08lx)", fmpgno, fmpgno );
    WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ] = { L'\0' };
    if ( m_fPersisted && ( m_pfapi->ErrPath( wszFmFilePath ) < JET_errSuccess ) )
    {
        wszFmFilePath[ 0 ] = L'\0';
    }
    WCHAR wszDbTimeMaxFm[ 256 ];
    OSStrCbFormatW( wszDbTimeMaxFm, sizeof( wszDbTimeMaxFm ), L"%I64u (0x%016I64x)", dbtimeMaxFm, dbtimeMaxFm );
    WCHAR wszPgno[ 64 ];
    OSStrCbFormatW( wszPgno, sizeof( wszPgno ), L"%ld (0x%08lx)", pgno, pgno );
    WCHAR wszDbTimeMaxDb[ 256 ];
    OSStrCbFormatW( wszDbTimeMaxDb, sizeof( wszDbTimeMaxDb ), L"%I64u (0x%016I64x)", dbtimeMaxDb, dbtimeMaxDb );

    const WCHAR* rgpwsz[] = { wszFmPgno, wszFmFilePath, wszDbTimeMaxFm, wszPgno, wszDbTimeMaxDb };

    UtilReportEvent(
        eventError,
        GENERAL_CATEGORY,
        FLUSHMAP_DBTIME_MISMATCH_ID,
        _countof( rgpwsz ),
        rgpwsz,
        0,
        NULL,
        m_pinst );
}

CFlushMap::CFlushMap() :
    m_lgenTargetedMax( 0 ),
    m_fInitialized( fFalse ),
    m_fGetSetAllowed( fFalse ),
    m_fDumpMode( fFalse ),
    m_fPersisted_( fFalse ),
    m_fPersisted( fFalse ),
    m_fCreateNew_( fFalse ),
    m_fCreateNew( fFalse ),
    m_fRecoverable( fFalse ),
    m_fReadOnly( fFalse ),
    m_fCleanForTerm( fFalse ),
    m_pinst( pinstNil ),
    m_pfsapi( NULL ),
    m_pfapi( NULL ),
    m_cInFlightIo( lMin ),
    m_sxwlSectionFlush( CLockBasicInfo( CSyncBasicInfo( "CFlushMap::m_sxwlSectionFlush" ), rankFlushMapAsyncWrite, 0 ) ),
    m_fmpgnoSectionFlushFirst( s_fmpgnoUninit ),
    m_fmpgnoSectionFlushLast( s_fmpgnoUninit ),
    m_fmpgnoSectionFlushNext( s_fmpgnoUninit ),
    m_fSectionCheckpointHeaderWrite( fFalse ),
    m_errSectionFlushWriteLastError( JET_errSuccess ),
    m_msSectionFlush(),
    m_groupSectionFlushWrite( CMeteredSection::groupInvalidNil ),
    m_pvSectionFlushAsyncWriteBuffer( NULL ),
    m_cbSectionFlushAsyncWriteBufferReserved( 0 ),
    m_cbSectionFlushAsyncWriteBufferCommitted( 0 ),
    m_ibSectionFlushAsyncWriteBufferNext( 0 ),
    m_cbLogGeneratedAtStartOfFlush( ullMax ),
    m_cbLogGeneratedAtLastFlushProgress( ullMax ),
    m_cbFmPageInMemory( 0 ),
    m_cbRuntimeBitmapPage( 0 ),
    m_cbDescriptorPage( 0 ),
    m_cDbPagesPerFlushMapPage( 0 ),
    m_fmdFmHdr( s_fmpgnoHdr ),
    m_fmdFmPg0( 0 ),
    m_critFmdGrowCapacity( CLockBasicInfo( CSyncBasicInfo( "CFlushMap::m_critFmdGrowCapacity" ), rankFlushMapGrowth, 0 ) ),
    m_rgfmd( NULL ),
    m_cfmdCommitted( 0 ),
    m_cfmpgAllocated( 0 ),
    m_cbfmdReserved( 0 ),
    m_cbfmdCommitted( 0 ),
    m_cbfmAllocated( 0 )
{
    m_sxwlSectionFlush.AcquireWriteLatch();
    m_sxwlSectionFlush.ReleaseOwnership( CSXWLatch::iWriteGroup );
    OnDebug( AssertFlushMapIoDisallowed_() );
}

CFlushMap::~CFlushMap()
{
    TermFlushMap_();
}

void CFlushMap::SetPersisted( __in const BOOL fPersisted )
{
    m_fPersisted_ = fPersisted;
}

void CFlushMap::SetCreateNew( __in const BOOL fCreateNew )
{
    m_fCreateNew_ = fCreateNew;
}

ERR CFlushMap::ErrInitFlushMap()
{
    ERR err = JET_errSuccess;

    Assert( !m_fInitialized );
    Assert( m_sxwlSectionFlush.FWriteLatched() );
    Assert( m_sxwlSectionFlush.FNotOwner() );

    m_lgenTargetedMax = 0;

    m_pinst = PinstGetInst_();
    m_fPersisted = m_fPersisted_;
    m_fCreateNew = m_fCreateNew_;
    m_fRecoverable = FIsRecoverable_();
    m_fReadOnly = FIsReadOnly_();

    m_cbFmPageInMemory = roundup( s_cbFlushMapPageOnDisk, OSMemoryPageCommitGranularity() );
    Expected( m_cbFmPageInMemory == s_cbFlushMapPageOnDisk );
    Assert( m_cbFmPageInMemory >= s_cbFlushMapPageOnDisk );
    Assert( ( m_cbFmPageInMemory % s_cbFlushMapPageOnDisk ) == 0 );
    
    m_cbDescriptorPage = OSMemoryPageCommitGranularity();
    m_cDbPagesPerFlushMapPage = (USHORT)( ( 8 * ( s_cbFlushMapPageOnDisk - sizeof( FlushMapDataPageHdr ) ) ) / s_cbitFlushType );
    m_cbRuntimeBitmapPage = roundup( roundupdiv( 1 * m_cDbPagesPerFlushMapPage, 8 ), OSMemoryPageCommitGranularity() );

    Assert( m_fmdFmHdr.pvWriteBuffer == NULL );
    Assert( m_fmdFmHdr.pv == NULL );

    if ( m_fPersisted )
    {
        Call( ErrAllocateFmPage_( &m_fmdFmHdr ) );
        memset( m_fmdFmHdr.pv, 0, sizeof( FMFILEHDR ) );

        if ( !m_fReadOnly )
        {
            const CFMPG cfmpgWriteMax = (CFMPG)UlFunctionalMax( ( (DWORD)UlParam( JET_paramMaxCoalesceWriteSize ) / s_cbFlushMapPageOnDisk ), 1 );
            const DWORD cbAsyncWriteBufferReserved = cfmpgWriteMax * m_cbFmPageInMemory;
            Alloc( m_pvSectionFlushAsyncWriteBuffer = PvOSMemoryPageReserve( cbAsyncWriteBufferReserved, NULL ) );
            m_cbSectionFlushAsyncWriteBufferReserved = cbAsyncWriteBufferReserved;
            m_cbSectionFlushAsyncWriteBufferCommitted = 0;
            m_ibSectionFlushAsyncWriteBufferNext = 0;
        }

        Call( ErrAttachFlushMap_() );
    }

    if ( !m_fDumpMode && !m_fPersisted )
    {
        Call( ErrSetFlushMapCapacity( 1 ) );
    }

    Assert( m_fmdFmHdr.FValid() || !m_fPersisted );

#ifdef DEBUG
    if ( m_fDumpMode )
    {
        Assert( m_cfmdCommitted == 0 );
        Assert( m_cfmpgAllocated == 0 );
    }
    else
    {
        Assert( m_cfmdCommitted >= 0 );
        Assert( m_cfmpgAllocated > 0 );
    }

    for ( FMPGNO fmpgno = 0; fmpgno < m_cfmpgAllocated; fmpgno++ )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
        Assert( pfmd->FAllocated() );
        Assert( pfmd->FValid() );
    }

    for ( FMPGNO fmpgno = m_cfmpgAllocated; fmpgno < m_cfmdCommitted; fmpgno++ )
    {
        FlushMapPageDescriptor* pfmd = NULL;
        CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
        Assert( !pfmd->FAllocated() );
        Assert( !pfmd->FValid() );
        Assert( !pfmd->FDirty() );
    }
#endif

HandleError:

    if ( err >= JET_errSuccess )
    {
        m_fInitialized = fTrue;
        m_fGetSetAllowed = fTrue;

        if ( m_fPersisted && !m_fReadOnly )
        {
            Assert( m_sxwlSectionFlush.FWriteLatched() );
            Assert( m_sxwlSectionFlush.FNotOwner() );
            m_sxwlSectionFlush.ClaimOwnership( CSXWLatch::iWriteGroup );
            m_sxwlSectionFlush.ReleaseWriteLatch();
        }
    }
    else
    {
        TermFlushMap_();
    }

    return err;
}

void CFlushMap::TermFlushMap()
{
    TermFlushMap_();
}

ERR CFlushMap::ErrCleanFlushMap()
{
    ERR err = JET_errSuccess;
    CSXWLatch::iGroup igroupFmHdr = CSXWLatch::iNoGroup;

    if ( !m_fPersisted )
    {
        m_fCleanForTerm = fTrue;
        return JET_errSuccess;
    }

    Assert( !m_fReadOnly );

    m_sxwlSectionFlush.AcquireWriteLatch();

    BOOL fCleanFileState = m_fCleanForTerm;
    if ( m_fCleanForTerm )
    {
        goto HandleError;
    }

    Call( ErrFlushAllSections_( OnDebug2( fTrue, fFalse ) ) );

    m_fmdFmHdr.sxwl.AcquireWriteLatch();
    igroupFmHdr = CSXWLatch::iWriteGroup;

    Call( ErrUtilFlushFileBuffers( m_pfapi, iofrFmTerm ) );

    FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)( m_fmdFmHdr.pvWriteBuffer );
    LGIGetDateTime( &pfmfilehdr->logtimeDetach );
    pfmfilehdr->lGenMinRequired = 0;
    pfmfilehdr->lGenMinRequiredTarget = 0;
    pfmfilehdr->SetClean();

    m_fmdFmHdr.SetDirty( m_pinst );

    err = ErrSyncWriteFmPage_( &m_fmdFmHdr );
    igroupFmHdr = CSXWLatch::iExclusiveGroup;
    fCleanFileState = ( err >= JET_errSuccess );

    m_fmdFmHdr.sxwl.ReleaseExclusiveLatch();
    igroupFmHdr = CSXWLatch::iNoGroup;

    Call( err );

    Call( ErrUtilFlushFileBuffers( m_pfapi, iofrFmTerm ) );

HandleError:

    Expected( ( igroupFmHdr == CSXWLatch::iNoGroup ) || ( err < JET_errSuccess ) );
    m_fmdFmHdr.sxwl.ReleaseLatch( igroupFmHdr );
    igroupFmHdr = CSXWLatch::iNoGroup;

    Assert( m_fmdFmHdr.sxwl.FNotOwner() );
    Assert( m_sxwlSectionFlush.FOwnWriteLatch() );

    m_fCleanForTerm = ( err >= JET_errSuccess );

    m_sxwlSectionFlush.ReleaseWriteLatch();

#ifdef DEBUG
    if ( fCleanFileState )
    {
        const FMPGNO fmpgnoUsedMax = m_cfmpgAllocated - 1;
        for ( FMPGNO fmpgno = s_fmpgnoHdr; fmpgno <= fmpgnoUsedMax; fmpgno++ )
        {
            FlushMapPageDescriptor* pfmd = NULL;
            CallS( ErrGetDescriptorFromFmPgno_( fmpgno, &pfmd ) );
            AssertSz( pfmd->FAllocated(), "Page must be allocated." );
            AssertSz( pfmd->FValid(), "Page must be valid." );
            AssertSz( !pfmd->FDirty(), "Page must be clean." );
        }
    }
#endif

    Assert( m_sxwlSectionFlush.FNotOwner() );

    return err;
}

ERR CFlushMap::ErrFlushMapFlushFileBuffers( _In_ const IOFLUSHREASON iofr )
{
    Assert( m_pfapi );
    return ErrUtilFlushFileBuffers( m_pfapi, iofr );
}

void CFlushMap::SetPgnoFlushType( __in const PGNO pgno, __in const CPAGE::PageFlushType pgft, __in const DBTIME dbtime )
{
    (void)ErrSetRangePgnoFlushType_( pgno, 1, pgft, dbtime, fFalse );
}

void CFlushMap::SetPgnoFlushType( __in const PGNO pgno, __in const CPAGE::PageFlushType pgft )
{
    SetPgnoFlushType( pgno, pgft, dbtimeNil );
}

void CFlushMap::SetRangeFlushType( __in const PGNO pgnoFirst, __in const CPG cpg, __in const CPAGE::PageFlushType pgft )
{
    (void)ErrSetRangePgnoFlushType_( pgnoFirst, cpg, pgft, dbtimeNil, fFalse );
}

ERR CFlushMap::ErrSetPgnoFlushTypeAndWait( __in const PGNO pgno, __in const CPAGE::PageFlushType pgft, __in const DBTIME dbtime )
{
    return ErrSetRangePgnoFlushType_( pgno, 1, pgft, dbtime, fTrue );
}

ERR CFlushMap::ErrSetRangeFlushTypeAndWait( __in const PGNO pgnoFirst, __in const CPG cpg, __in const CPAGE::PageFlushType pgft )
{
    return ErrSetRangePgnoFlushType_( pgnoFirst, cpg, pgft, dbtimeNil, fTrue );
}

CPAGE::PageFlushType CFlushMap::PgftGetPgnoFlushType( __in const PGNO pgno )
{
    return PgftGetPgnoFlushType( pgno, dbtimeNil, NULL );
}

CPAGE::PageFlushType CFlushMap::PgftGetPgnoFlushType( __in const PGNO pgno, __in const DBTIME dbtime, __out BOOL* const pfRuntime )
{
    ERR err = JET_errSuccess;
    FlushMapPageDescriptor* pfmd = NULL;
    CPAGE::PageFlushType pgft = CPAGE::pgftUnknown;
    DBTIME dbtimeMax = dbtimeNil;
    FMPGNO fmpgno = s_fmpgnoUninit;

    AssertSz( m_fGetSetAllowed && !m_fDumpMode, "Getting flush type not allowed in CFlushMap::PgftGetPgnoFlushType()." );

    Call( ErrGetDescriptorFromPgno_( pgno, &pfmd ) );

    if ( !pfmd->FValid() )
    {
        Error( ErrERRCheck( JET_errInvalidOperation ) );
    }

    Assert( pfmd->FValid() );
    Assert( pfmd->sxwl.FNotOwner() );
    pfmd->sxwl.AcquireSharedLatch();

    pgft = PgftGetFlushType_( pfmd, pgno );

    if ( pfRuntime != NULL )
    {
        *pfRuntime = FGetFlushTypeRuntime_( pfmd, pgno );
    }

    dbtimeMax = pfmd->dbtimeMax;

    fmpgno = pfmd->fmpgno;
    pfmd->sxwl.ReleaseSharedLatch();

    if ( ( pgft != CPAGE::pgftUnknown ) && ( dbtime != dbtimeNil ) && ( dbtimeMax != dbtimeNil )  && ( dbtime > dbtimeMax ) )
    {
        AssertSz( FNegTest( fCorruptingWithLostFlush ), "Inconsistent DBTIMEs in the flush map (fmpgno = %ld, dbtimeMax = %I64u, pgno = %lu, dbtime = %I64u).", fmpgno, dbtimeMax, pgno, dbtime );

        LogEventFmInconsistentDbTime_( fmpgno, dbtimeMax, pgno, dbtime );

        pfmd->sxwl.AcquireWriteLatch();
        InitializeFmDataPage_( pfmd );
        pfmd->SetDirty( m_pinst );

        if ( m_fPersisted )
        {
            (void)ErrSyncWriteFmPage_( pfmd );
            pfmd->sxwl.ReleaseExclusiveLatch();
        }
        else
        {
            pfmd->sxwl.ReleaseWriteLatch();
        }

        pgft = CPAGE::pgftUnknown;
    }

HandleError:

    return pgft;
}

ERR CFlushMap::ErrSyncRangeInvalidateFlushType( __in const PGNO pgnoFirst, __in const CPG cpg )
{
    ERR err = JET_errSuccess;

    Assert( pgnoFirst != pgnoNull );
    Assert( cpg >= 0 );
    Expected( cpg > 0 );
    const PGNO pgnoLast = pgnoFirst + cpg - 1;

    Call( ErrSetFlushMapCapacity( pgnoLast ) );
    if ( FRecoverable() )
    {
        Call( ErrSetRangeFlushTypeAndWait( pgnoFirst, cpg, CPAGE::pgftUnknown ) );
    }
    else
    {
        SetRangeFlushType( pgnoFirst, cpg, CPAGE::pgftUnknown );
    }

HandleError:
    return err;
}

LONG CFlushMap::LGetFmGenMinRequired()
{
    Assert( !m_fReadOnly && !m_fDumpMode );
    
    if ( m_fReadOnly || !m_fRecoverable )
    {
        return lMax;
    }

    Assert( m_fmdFmHdr.sxwl.FNotOwner() );

    m_fmdFmHdr.sxwl.AcquireSharedLatch();
    if ( m_fmdFmHdr.FRecentlyAsyncWrittenHeader() && ( ErrUtilFlushFileBuffers( m_pfapi, iofrFmFlush ) >= JET_errSuccess ) )
    {
        m_fmdFmHdr.ResetRecentlyAsyncWrittenHeader();
    }

    const LONG lGenMinRequired = ( (FMFILEHDR*)( m_fmdFmHdr.pv ) )->lGenMinRequired;
    m_fmdFmHdr.sxwl.ReleaseSharedLatch();

    return lGenMinRequired;
}

BOOL CFlushMap::FRecoverable()
{
    return m_fRecoverable;
}


BOOL CFlushMap::FRequestFmSectionWrite( __in const QWORD cbLogGenerated, __in const QWORD cbPreferredChkptDepth )
{
    BOOL fRequest = fFalse;
    BOOL fOwnLatch = fFalse;


    if ( m_sxwlSectionFlush.ErrTryAcquireSharedLatch() != CSXWLatch::ERR::errSuccess )
    {
        goto HandleError;
    }
    fOwnLatch = fTrue;

    Assert( m_fInitialized && m_fPersisted && !m_fDumpMode && !m_fReadOnly );

    if ( !m_fRecoverable )
    {
        goto HandleError;
    }

    if ( m_fCleanForTerm )
    {
        goto HandleError;
    }

    if ( ( cbPreferredChkptDepth == 0 ) &&
        ( ( m_cbLogGeneratedAtLastFlushProgress != ullMax ) &&
        ( cbLogGenerated <= m_cbLogGeneratedAtLastFlushProgress ) ) ||
        ( ( m_cbLogGeneratedAtStartOfFlush != ullMax ) &&
                ( cbLogGenerated <= m_cbLogGeneratedAtStartOfFlush ) )  )
    {
        goto HandleError;
    }

    const QWORD cbChkptDepthFlushThreshold = (QWORD)( (double)cbPreferredChkptDepth * ( (double)s_pctCheckpointDelay / 100.0 ) );
    QWORD cbFlushMapFlushedProgress;
    if ( m_cbLogGeneratedAtLastFlushProgress == ullMax )
    {
        cbFlushMapFlushedProgress = ( m_cbLogGeneratedAtStartOfFlush != ullMax ) ?
            ( m_cbLogGeneratedAtStartOfFlush + cbChkptDepthFlushThreshold ) :
            0;
    }
    else
    {
        Assert( m_cbLogGeneratedAtStartOfFlush != ullMax );
        Assert( m_cbLogGeneratedAtStartOfFlush <= m_cbLogGeneratedAtLastFlushProgress );
        
        ExpectedSz( m_cfmpgAllocated > 0, "A properly initialized flush map should have at least one allocated page." );
        const CFMPG cfmpgTotal = m_cfmpgAllocated + 2;
        const CFMPG cfmpgFlushed = ( ( m_fmpgnoSectionFlushNext >= 0 ) ? m_fmpgnoSectionFlushNext : 0 ) + 1;

        cbFlushMapFlushedProgress = m_cbLogGeneratedAtStartOfFlush + ( cbChkptDepthFlushThreshold * cfmpgFlushed ) / cfmpgTotal;
    }

    fRequest = ( cbLogGenerated >= cbFlushMapFlushedProgress );

HandleError:

    if ( fOwnLatch )
    {
        m_sxwlSectionFlush.ReleaseSharedLatch();
    }
    Assert( m_sxwlSectionFlush.FNotOwner() );

    return fRequest;
}

ERR CFlushMap::ErrFlushAllSections( OnDebug( __in const BOOL fMapPatching ) )
{
    ERR err = JET_errSuccess;

    if ( !m_fPersisted )
    {
        return JET_errSuccess;
    }

    Assert( !m_fReadOnly );

    m_sxwlSectionFlush.AcquireWriteLatch();

    Call( ErrFlushAllSections_( OnDebug2( fFalse, fMapPatching ) ) );

HandleError:

    Assert( m_sxwlSectionFlush.FOwnWriteLatch() );
    m_sxwlSectionFlush.ReleaseWriteLatch();

    return err;
}

void CFlushMap::FlushOneSection( __in const QWORD cbLogGenerated )
{
    BOOL fOwnLatch = fFalse;

    if ( m_sxwlSectionFlush.ErrTryAcquireWriteLatch() != CSXWLatch::ERR::errSuccess )
    {
        goto HandleError;
    }
    fOwnLatch = fTrue;

    Assert( m_fInitialized && m_fPersisted && !m_fDumpMode && !m_fReadOnly );
    Expected( m_fRecoverable );

    if ( m_fCleanForTerm )
    {
        goto HandleError;
    }

    if ( m_cbLogGeneratedAtLastFlushProgress == ullMax )
    {
        m_cbLogGeneratedAtStartOfFlush = cbLogGenerated;
    }
    m_cbLogGeneratedAtLastFlushProgress = cbLogGenerated;

    FlushOneSection_( OnDebug2( fFalse, fFalse ) );
    fOwnLatch = fFalse;
    Assert( m_sxwlSectionFlush.FNotOwner() );

HandleError:

    if ( fOwnLatch )
    {
        m_sxwlSectionFlush.ReleaseWriteLatch();
    }
    Assert( m_sxwlSectionFlush.FNotOwner() );
}

ERR CFlushMap::ErrSetFlushMapCapacity( __in const PGNO pgnoReq )
{
    JET_ERR err = JET_errSuccess;
    BOOL fInCritSec = fFalse;
    CFMPG cfmpgNeeded = CfmpgGetRequiredFmDataPageCount_( pgnoReq );

    if ( m_fDumpMode )
    {
        AssertSz( fFalse, "Should not be trying to change flush map capacity if read-only or dump mode." );
        Error( ErrERRCheck( JET_errInvalidOperation ) );
    }

    if ( m_fPersisted && !m_fReadOnly )
    {
        QWORD cbFmFileSizeActual = 0;
        QWORD cbFmFileSizeNeeded = CbGetRequiredFmFileSize_( cfmpgNeeded );

        m_critFmdGrowCapacity.Enter();
        fInCritSec = fTrue;
        Call( m_pfapi->ErrSize( &cbFmFileSizeActual, IFileAPI::filesizeLogical ) );

        if ( cbFmFileSizeNeeded > cbFmFileSizeActual )
        {
            const CFMPG cfmpgPreferredNeeded = CfmpgGetPreferredFmDataPageCount_( pgnoReq );
            Assert( cfmpgPreferredNeeded >= cfmpgNeeded );
            cfmpgNeeded = cfmpgPreferredNeeded;
            cbFmFileSizeNeeded = CbGetRequiredFmFileSize_( cfmpgNeeded );
            TraceContextScope tcScope( iorpFlushMap );
            Call( m_pfapi->ErrSetSize( *tcScope, cbFmFileSizeNeeded, fTrue, QosSyncDefault( m_pinst ) ) );
        }

        m_critFmdGrowCapacity.Leave();
        fInCritSec = fFalse;
    }

    Call( ErrAllocateDescriptorsCapacity_( cfmpgNeeded ) );
    Call( ErrAllocateFmDataPageCapacity_( cfmpgNeeded ) );
    InitializeFmDataPageCapacity_( cfmpgNeeded );

HandleError:

    if ( fInCritSec )
    {
        m_critFmdGrowCapacity.Leave();
    }

    return err;
}

INLINE FMPGNO CFlushMap::FmpgnoGetFmPgnoFromDbPgno( __in const PGNO pgno )
{
    Assert( pgno <= pgnoSysMax );

#ifndef ENABLE_JET_UNIT_TEST
    Expected( pgno >= 1 );
#endif

    return ( pgno / m_cDbPagesPerFlushMapPage );
}

void CFlushMap::EnterDbHeaderFlush( __in CFlushMap* const pfm, __out SIGNATURE* const psignDbHdrFlush, __out SIGNATURE* const psignFlushMapHdrFlush )
{
    if ( ( pfm != NULL ) && pfm->m_fInitialized )
    {
        pfm->EnterDbHeaderFlush_( psignDbHdrFlush, psignFlushMapHdrFlush );
    }
    else
    {
        SIGGetSignature( psignDbHdrFlush );
        SIGResetSignature( psignFlushMapHdrFlush );
    }
}

void CFlushMap::LeaveDbHeaderFlush( __in CFlushMap* const pfm )
{
    if ( ( pfm != NULL ) && pfm->m_fInitialized )
    {
        pfm->LeaveDbHeaderFlush_();
    }
}


BOOL CFlushMapForAttachedDb::FIsReadOnly_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->FReadOnlyAttach();
}

ERR CFlushMapForAttachedDb::ErrGetFmFilePath_( _Out_writes_z_(OSFSAPI_MAX_PATH) WCHAR* const wszFmFilePath )
{
    Assert( m_pfmp != NULL );
    return CFlushMap::ErrGetFmPathFromDbPath( wszFmFilePath, m_pfmp->WszDatabaseName() );
}

BOOL CFlushMapForAttachedDb::FIsRecoverable_()
{
    Assert( m_pfmp != NULL );
    return ( m_fPersisted && m_pfmp->FLogOn() );
}
    
INST* CFlushMapForAttachedDb::PinstGetInst_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->Pinst();
}

IFileSystemConfiguration* CFlushMapForAttachedDb::Pfsconfig_()
{
    return PinstGetInst_()->m_pfsconfig;
}

QWORD CFlushMapForAttachedDb::QwGetEseFileId_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->Ifmp();
}

ULONG CFlushMapForAttachedDb::UlGetDbState_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->Pdbfilehdr()->le_dbstate;
}

LONG CFlushMapForAttachedDb::LGetDbGenMinRequired_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->Pdbfilehdr()->le_lGenMinRequired;
}

LONG CFlushMapForAttachedDb::LGetDbGenMinConsistent_()
{
    Assert( m_pfmp != NULL );
    return m_pfmp->Pdbfilehdr()->le_lGenMinConsistent;
}

void CFlushMapForAttachedDb::CopyDbHdrFlushSignatureFromDb_( SIGNATURE* const psignDbHdrFlushFromFm )
{
    Assert( m_pfmp != NULL );
    Assert( m_fPersisted );
    UtilMemCpy( psignDbHdrFlushFromFm, &m_pfmp->Pdbfilehdr()->signDbHdrFlush, sizeof( SIGNATURE ) );
}

void CFlushMapForAttachedDb::GetFlushSignaturesFromDb_( const FMFILEHDR* const pfmfilehdr, SIGNATURE* const psignDbHdrFlushFromDb, SIGNATURE* const psignFlushMapHdrFlushFromDb )
{
    Assert( pfmfilehdr != NULL );
    Assert( psignDbHdrFlushFromDb != NULL );
    Assert( psignFlushMapHdrFlushFromDb != NULL );
    Assert( m_pfmp != NULL );
    PdbfilehdrReadOnly pdbfilehdr = m_pfmp->Pdbfilehdr();
    UtilMemCpy( psignDbHdrFlushFromDb, &pdbfilehdr->signDbHdrFlush, sizeof( SIGNATURE ) );
    UtilMemCpy( psignFlushMapHdrFlushFromDb, &pdbfilehdr->signFlushMapHdrFlush, sizeof( SIGNATURE ) );
}

CPG CFlushMapForAttachedDb::CpgGetDbExtensionSize_()
{
    Assert( m_pinst != NULL );
    return (CPG)UlParam( m_pinst, JET_paramDbExtensionSize );
}

CFlushMapForAttachedDb::CFlushMapForAttachedDb() :
    CFlushMap(),
    m_pfmp( NULL )
{
}

void CFlushMapForAttachedDb::SetDbFmp( __in FMP* const pfmp )
{
    Assert( !m_fInitialized );
    Assert( m_pfmp == NULL );
    Assert( pfmp != NULL );
    m_pfmp = pfmp;
}



BOOL CFlushMapForUnattachedDb::FIsReadOnly_()
{
    return m_fReadOnly_;
}

ERR CFlushMapForUnattachedDb::ErrGetFmFilePath_( _Out_writes_z_(OSFSAPI_MAX_PATH) WCHAR* const wszFmFilePath )
{
    Assert( wszFmFilePath != NULL );
    Assert( m_wszFmFilePath_ != NULL );
    return ErrOSStrCbCopyW( wszFmFilePath, sizeof( WCHAR ) * IFileSystemAPI::cchPathMax, m_wszFmFilePath_ );
}

BOOL CFlushMapForUnattachedDb::FIsRecoverable_()
{
    return m_fRecoverable_;
}

INST* CFlushMapForUnattachedDb::PinstGetInst_()
{
    return m_pinst_;
}

IFileSystemConfiguration* CFlushMapForUnattachedDb::Pfsconfig_()
{
    return m_pinst_ ? m_pinst_->m_pfsconfig : m_pfsconfig_;
}

QWORD CFlushMapForUnattachedDb::QwGetEseFileId_()
{
    return 0;
}

ULONG CFlushMapForUnattachedDb::UlGetDbState_()
{
    return m_ulDbState_;
}

LONG CFlushMapForUnattachedDb::LGetDbGenMinRequired_()
{
    return m_lGenDbMinRequired_;
}

LONG CFlushMapForUnattachedDb::LGetDbGenMinConsistent_()
{
    return m_lGenDbMinConsistent_;
}

void CFlushMapForUnattachedDb::CopyDbHdrFlushSignatureFromDb_( SIGNATURE* const psignDbHdrFlushFromFm )
{
    Assert( psignDbHdrFlushFromFm != NULL );
    Assert( m_psignDbHdrFlushFromDb_ != NULL );
    Assert( m_fPersisted );
    UtilMemCpy( psignDbHdrFlushFromFm, m_psignDbHdrFlushFromDb_, sizeof( SIGNATURE ) );
}

void CFlushMapForUnattachedDb::GetFlushSignaturesFromDb_( const FMFILEHDR* const pfmfilehdr, SIGNATURE* const psignDbHdrFlushFromDb, SIGNATURE* const psignFlushMapHdrFlushFromDb )
{
    Assert( pfmfilehdr != NULL );
    Assert( psignDbHdrFlushFromDb != NULL );
    Assert( psignFlushMapHdrFlushFromDb != NULL );
    Assert( m_psignDbHdrFlushFromDb_ != NULL );
    Assert( m_psignDbHdrFlushFromDb_ != NULL );
    UtilMemCpy( psignDbHdrFlushFromDb, m_psignDbHdrFlushFromDb_, sizeof( SIGNATURE ) );
    UtilMemCpy( psignFlushMapHdrFlushFromDb, m_psignFlushMapHdrFlushFromDb_, sizeof( SIGNATURE ) );
}

CPG CFlushMapForUnattachedDb::CpgGetDbExtensionSize_()
{
    if ( m_pinst_ != pinstNil )
    {
        return (CPG)UlParam( m_pinst_, JET_paramDbExtensionSize );
    }

    return m_cpgDbExtensionSize_;
}

CFlushMapForUnattachedDb::CFlushMapForUnattachedDb() :
    CFlushMap(),
    m_fReadOnly_( fFalse ),
    m_wszFmFilePath_( NULL ),
    m_fRecoverable_( fFalse ),
    m_pinst_( pinstNil ),
    m_pfsconfig_( NULL ),
    m_ulDbState_( 0 ),
    m_lGenDbMinRequired_( 0 ),
    m_lGenDbMinConsistent_( 0 ),
    m_psignDbHdrFlushFromDb_( NULL ),
    m_psignFlushMapHdrFlushFromDb_( NULL ),
    m_cpgDbExtensionSize_( 0 )
{
    m_fPersisted_ = fTrue;
    m_fCreateNew_ = fFalse;
}

void CFlushMapForUnattachedDb::SetReadOnly( __in const BOOL fReadOnly )
{
    m_fReadOnly_ = fReadOnly;
}

void CFlushMapForUnattachedDb::SetFmFilePath( __in const WCHAR* const wszFmFilePath )
{
    m_wszFmFilePath_ = wszFmFilePath;
}

void CFlushMapForUnattachedDb::SetRecoverable( __in const BOOL fRecoverable )
{
    m_fRecoverable_ = fRecoverable;
}

void CFlushMapForUnattachedDb::SetEseInstance( __in INST* const pinst )
{
    m_pinst_ = pinst;
}

void CFlushMapForUnattachedDb::SetFileSystemConfiguration( __in IFileSystemConfiguration* const pfsconfig )
{
    m_pfsconfig_ = pfsconfig;
}

void CFlushMapForUnattachedDb::SetDbState( __in const ULONG ulDbState )
{
    m_ulDbState_ = ulDbState;
}

void CFlushMapForUnattachedDb::SetDbGenMinRequired( __in const LONG lGenDbMinRequired )
{
    m_lGenDbMinRequired_ = lGenDbMinRequired;
}

void CFlushMapForUnattachedDb::SetDbGenMinConsistent( __in const LONG lGenDbMinConsistent )
{
    m_lGenDbMinConsistent_ = lGenDbMinConsistent;
}

void CFlushMapForUnattachedDb::SetDbHdrFlushSignaturePointerFromDb( __in const SIGNATURE* const psignDbHdrFlushFromDb )
{
    m_psignDbHdrFlushFromDb_ = psignDbHdrFlushFromDb;
}

void CFlushMapForUnattachedDb::SetFmHdrFlushSignaturePointerFromDb( __in const SIGNATURE* const psignFlushMapHdrFlushFromDb )
{
    m_psignFlushMapHdrFlushFromDb_ = psignFlushMapHdrFlushFromDb;
}

void CFlushMapForUnattachedDb::SetDbExtensionSize( __in const CPG cpgDbExtensionSize )
{
    m_cpgDbExtensionSize_ = cpgDbExtensionSize;
}

ERR CFlushMapForUnattachedDb::ErrGetPersistedFlushMapOrNullObjectIfRuntime(
    _In_z_ const WCHAR* const wszDbFilePath,
    __in const DBFILEHDR* const pdbHdr,
    __in INST* const pinst,
    __out CFlushMapForUnattachedDb** const ppfm )
{
    ERR err = JET_errSuccess;
    CFlushMapForUnattachedDb* pfm = NULL;
    WCHAR wszFmFilePath[ IFileSystemAPI::cchPathMax ] = { L'\0' };
    BOOL fNullObjectWithSuccess = fFalse;

    Call( CFlushMap::ErrGetFmPathFromDbPath( wszFmFilePath, wszDbFilePath ) );
    Alloc( pfm = new CFlushMapForUnattachedDb() );

    pfm->SetPersisted( fTrue );
    pfm->SetReadOnly( fTrue );
    pfm->SetFmFilePath( wszFmFilePath );
    pfm->SetCreateNew( fFalse );
    pfm->SetRecoverable( fTrue );
    pfm->SetEseInstance( pinst );
    pfm->SetDbState( pdbHdr->le_dbstate );
    pfm->SetDbGenMinRequired( pdbHdr->le_lGenMinRequired );
    pfm->SetDbGenMinConsistent( pdbHdr->le_lGenMinConsistent );
    pfm->SetDbHdrFlushSignaturePointerFromDb( &pdbHdr->signDbHdrFlush );
    pfm->SetFmHdrFlushSignaturePointerFromDb( &pdbHdr->signFlushMapHdrFlush );
    Call( pfm->ErrInitFlushMap() );

    if ( !pfm->m_fPersisted )
    {
        fNullObjectWithSuccess = fTrue;
        goto HandleError;
    }

    pfm->TermFlushMap();

    pfm->SetReadOnly( fFalse );
    Call( pfm->ErrInitFlushMap() );

    if ( !pfm->m_fPersisted )
    {
        fNullObjectWithSuccess = fTrue;
        goto HandleError;
    }

HandleError:

    if ( ( err < JET_errSuccess ) || fNullObjectWithSuccess )
    {
        delete pfm;
        pfm = NULL;
    }

    Assert( ppfm != NULL );
    *ppfm = pfm;

    Assert( ( err >= JET_errSuccess ) || ( *ppfm == NULL ) );

    return err;
}



void CFlushMapForDump::DumpFmHdr_( const FlushMapPageDescriptor* const pfmd )
{
    Assert( pfmd->FIsFmHeader() );
    
    const void* const pv = pfmd->pvWriteBuffer;
    const BOOL fEmpty = !CPAGE::FPageIsInitialized( pv, s_cbFlushMapPageOnDisk );

    if ( fEmpty )
    {
        DUMPPrintF( "\n" );
        DUMPPrintF( "Flush map file header is empty.\n" );
        return;
    }

    const FMFILEHDR* const pfmfilehdr = (FMFILEHDR*)pv;
    XECHECKSUM checksumPersisted = 0, checksumCalculated = 0;

    const ERR err = ErrChecksumFmPage_( (void* const)pv, pfmd->fmpgno, fFalse, &checksumPersisted, &checksumCalculated );
    Assert( ( err == JET_errSuccess ) || ( err == JET_errReadVerifyFailure ) );
    Assert( (LONG)checksumPersisted == pfmfilehdr->checksum );

    DUMPPrintF( "\n" );
    DUMPPrintF( "Checksum Information:\n" );
    DUMPPrintF( "                  Persisted Checksum: 0x%016I64x\n", checksumPersisted );
    DUMPPrintF( "                 Calculated Checksum: 0x%016I64x\n", checksumCalculated );

    DUMPPrintF( "\n" );
    DUMPPrintF( "Fields:\n" );
    DUMPPrintF( "                             Version: %lu.%lu.%lu\n", (ULONG)pfmfilehdr->le_ulFmVersionMajor, (ULONG)pfmfilehdr->le_ulFmVersionUpdateMajor, (ULONG)pfmfilehdr->le_ulFmVersionUpdateMinor );
    DUMPPrintF( "                               Flags: 0x%08lx (Clean:%s, NewChecksumFormat:%s)\n",
        (ULONG)pfmfilehdr->fFlags,
        pfmfilehdr->FClean() ? "Yes" : "No",
        pfmfilehdr->FNewChecksumFormat() ? "Yes" : "No" );
    DUMPPrintF( "                           File Type: %lu\n", (ULONG)pfmfilehdr->filetype );
    DUMPPrintF( "                  Flush Map Creation: " ); DUMPPrintLogTime( &pfmfilehdr->logtimeCreate ); DUMPPrintF( "\n" );
    DUMPPrintF( "               Last Flush Map Attach: " ); DUMPPrintLogTime( &pfmfilehdr->logtimeAttach ); DUMPPrintF( "\n" );
    DUMPPrintF( "               Last Flush Map Detach: " ); DUMPPrintLogTime( &pfmfilehdr->logtimeDetach ); DUMPPrintF( "\n" );
    DUMPPrintF( "                   Log Required Min.: %ld (%#x)\n", (LONG)pfmfilehdr->lGenMinRequired, (LONG)pfmfilehdr->lGenMinRequired );
    DUMPPrintF( "            Log Required Min. (Next): %ld (%#x)\n", (LONG)pfmfilehdr->lGenMinRequiredTarget, (LONG)pfmfilehdr->lGenMinRequiredTarget );
    DUMPPrintF( "   Last Flush Map Full Flush Started: " ); DUMPPrintLogTime( &pfmfilehdr->logtimeFlushLastStarted ); DUMPPrintF( "\n" );
    DUMPPrintF( " Last Flush Map Full Flush Completed: " ); DUMPPrintLogTime( &pfmfilehdr->logtimeFlushLastCompleted ); DUMPPrintF( "\n" );
    DUMPPrintF( "           DB Header Flush Signature: " ); DUMPPrintSig( &pfmfilehdr->signDbHdrFlush );
    DUMPPrintF( "    Flush Map Header Flush Signature: " ); DUMPPrintSig( &pfmfilehdr->signFlushMapHdrFlush );
}

void CFlushMapForDump::DumpFmDataPage_( const FlushMapPageDescriptor* const pfmd, const BOOL fDumpFlushStates )
{
    Assert( pfmd->fmpgno >= 0 );
    
    const void* const pv = pfmd->pv;
    const BOOL fEmpty = !CPAGE::FPageIsInitialized( pv, s_cbFlushMapPageOnDisk );

    if ( fEmpty )
    {
        DUMPPrintF( "\n" );
        DUMPPrintF( "Flush map data page is empty.\n" );
        return;
    }

    const FlushMapDataPageHdr* const pfmpghdr = (FlushMapDataPageHdr*)pv;
    XECHECKSUM checksumPersisted = 0, checksumCalculated = 0;

    const ERR err = ErrChecksumFmPage_( (void* const)pv, pfmd->fmpgno, fFalse, &checksumPersisted, &checksumCalculated );
    Assert( ( err == JET_errSuccess ) || ( err == JET_errReadVerifyFailure ) );
    Assert( checksumPersisted == pfmpghdr->checksum );

    DUMPPrintF( "\n" );
    DUMPPrintF( "Checksum Information:\n" );
    DUMPPrintF( "                  Persisted Checksum: 0x%016I64x\n", checksumPersisted );
    DUMPPrintF( "                 Calculated Checksum: 0x%016I64x\n", checksumCalculated );

    DUMPPrintF( "\n" );
    DUMPPrintF( "Fields:\n" );
    DUMPPrintF( "                        DB Time Max.: %I64u (0x%016I64x)\n", (DBTIME)pfmpghdr->dbtimeMax, (DBTIME)pfmpghdr->dbtimeMax );
    DUMPPrintF( "               Flush Map Page Number: %ld (0x%08lx)\n", (PGNO)pfmpghdr->fmpgno, (PGNO)pfmpghdr->fmpgno );
    DUMPPrintF( "                               Flags: 0x%08lx (NewChecksumFormat:%s)\n",
        (ULONG)pfmpghdr->fFlags,
        pfmpghdr->FNewChecksumFormat() ? "Yes" : "No" );

    DUMPPrintF( "\n" );

    if ( fDumpFlushStates )
    {
        DUMPPrintF( "Flush States (%hu database pages):\n", m_cDbPagesPerFlushMapPage );
        DUMPPrintF( "PGNO      : FLUSH STATE" );

        const PGNO pgnoFirst = pfmd->fmpgno * m_cDbPagesPerFlushMapPage;
        for ( USHORT ifmpgno = 0; ifmpgno < m_cDbPagesPerFlushMapPage; ifmpgno++ )
        {
            const PGNO pgno = pgnoFirst + ifmpgno;

            if ( ( ifmpgno % 32 ) == 0 )
            {
                DUMPPrintF( "\n" );
                DUMPPrintF( "0x%08lx: ", pgno );
            }

            DUMPPrintF( "%d ", (INT)PgftGetFlushType_( (FlushMapPageDescriptor* const)pfmd, pgno ) );
        }
        DUMPPrintF( "\n" );
        DUMPPrintF( "\n" );
    }
}

BOOL CFlushMapForDump::FIsReadOnly_()
{
    return fTrue;
}

ERR CFlushMapForDump::ErrGetFmFilePath_( _Out_writes_z_(OSFSAPI_MAX_PATH) WCHAR* const wszFmFilePath )
{
    Assert( wszFmFilePath != NULL );
    Assert( m_wszFmFilePath_ != NULL );
    return ErrOSStrCbCopyW( wszFmFilePath, sizeof( WCHAR ) * IFileSystemAPI::cchPathMax, m_wszFmFilePath_ );
}

BOOL CFlushMapForDump::FIsRecoverable_()
{
    return fFalse;
}

INST* CFlushMapForDump::PinstGetInst_()
{
    return m_pinst_;
}

IFileSystemConfiguration* CFlushMapForDump::Pfsconfig_()
{
    return m_pinst_ ? m_pinst_->m_pfsconfig : m_pfsconfig_;
}

QWORD CFlushMapForDump::QwGetEseFileId_()
{
    return 0;
}

ULONG CFlushMapForDump::UlGetDbState_()
{
    Expected( fFalse );
    return 0;
}

LONG CFlushMapForDump::LGetDbGenMinRequired_()
{
    Expected( fFalse );
    return 0;
}

LONG CFlushMapForDump::LGetDbGenMinConsistent_()
{
    Expected( fFalse );
    return 0;
}

void CFlushMapForDump::CopyDbHdrFlushSignatureFromDb_( SIGNATURE* const psignDbHdrFlushFromFm )
{
    Expected( fFalse );
}

void CFlushMapForDump::GetFlushSignaturesFromDb_( const FMFILEHDR* const pfmfilehdr, SIGNATURE* const psignDbHdrFlushFromDb, SIGNATURE* const psignFlushMapHdrFlushFromDb )
{
    Expected( fFalse );
}

CPG CFlushMapForDump::CpgGetDbExtensionSize_()
{
    Expected( fFalse );
    return 0;
}

ERR CFlushMapForDump::ErrChecksumFlushMapFile( __in INST* const pinst, __in const WCHAR* const wszFmFilePath, __in_opt IFileSystemConfiguration* pfsconfig )
{
    ERR err = JET_errSuccess;
    FMPGNO fmpgno = s_fmpgnoHdr;
    BOOL fFinished = fFalse;
    CFlushMapForDump flushmap;
    CFMPG cfmpgTotal = 0;
    CFMPG cfmpgErr = 0;
    CFMPG cfmpgWrn = 0;

    DUMPPrintF( "\n" );

    flushmap.SetFmFilePath( wszFmFilePath );
    flushmap.SetEseInstance( pinst );
    flushmap.SetFileSystemConfiguration( pfsconfig );

    Call( flushmap.ErrInitFlushMap() );

    cfmpgTotal = -1;

    while ( !fFinished )
    {
        ERR errT = flushmap.ErrChecksumFmPage( fmpgno );
        const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );

        if ( errT == JET_errFileIOBeyondEOF )
        {
            fFinished = fTrue;
            
            Expected( !fFmHeaderPage );

            if ( fFmHeaderPage )
            {
                errT = ErrERRCheck( JET_errReadVerifyFailure );
            }
            else
            {
                errT = JET_errSuccess;
            }
        }
        else if ( errT == JET_errPageNotInitialized )
        {
            Expected( !fFmHeaderPage );

            if ( !fFmHeaderPage )
            {
                errT = JET_errSuccess;
            }
        }

        if ( errT < JET_errSuccess )
        {
            if ( err >= JET_errSuccess )
            {
                err = errT;
            }

            DUMPPrintF( "ERROR: page %ld (0x%08lx) failed verification with error code %ld.\n", fmpgno, fmpgno, errT );
            cfmpgErr++;
        }
        else if ( errT > JET_errSuccess )
        {
            DUMPPrintF( "WARNING: page %ld (0x%08lx) passed verification with error code %ld.\n", fmpgno, fmpgno, errT );
            cfmpgWrn++;
        }

        if ( fFmHeaderPage )
        {
            fmpgno = 0;
        }
        else
        {
            fmpgno++;
        }

        if ( ( fmpgno % 10 ) == 0 )
        {
            DUMPPrintF( "." );
        }

        cfmpgTotal++;
    }

HandleError:

    flushmap.TermFlushMap();

    DUMPPrintF( "\n" );
    DUMPPrintF( "%ld page(s) verified.\n", cfmpgTotal );
    DUMPPrintF( "%ld page(s) failed verification.\n", cfmpgErr );
    DUMPPrintF( "%ld page(s) passed verification with a warning.\n", cfmpgWrn );
    DUMPPrintF( "\n" );

    return err;
}

ERR CFlushMapForDump::ErrDumpFlushMapPage( __in INST* const pinst, __in const WCHAR* const wszFmFilePath, __in const FMPGNO fmpgno, __in const BOOL fDumpFlushStates, __in_opt IFileSystemConfiguration* pfsconfig )
{
    ERR err = JET_errSuccess;
    CFlushMapForDump flushmap;

    flushmap.SetFmFilePath( wszFmFilePath );
    flushmap.SetEseInstance( pinst );
    flushmap.SetFileSystemConfiguration( pfsconfig );

    Call( flushmap.ErrInitFlushMap() );
    Call( flushmap.ErrDumpFmPage( fmpgno, fDumpFlushStates ) );

HandleError:

    flushmap.TermFlushMap();

    return err;
}

CFlushMapForDump::CFlushMapForDump() :
    CFlushMap(),
    m_wszFmFilePath_( NULL ),
    m_pinst_( pinstNil ),
    m_pfsconfig_( NULL )
{
    m_fPersisted_ = fTrue;
    m_fCreateNew_ = fFalse;
    m_fDumpMode = fTrue;
}

void CFlushMapForDump::SetFmFilePath( __in const WCHAR* const wszFmFilePath )
{
    m_wszFmFilePath_ = wszFmFilePath;
}

void CFlushMapForDump::SetEseInstance( __in INST* const pinst )
{
    m_pinst_ = pinst;
}

void CFlushMapForDump::SetFileSystemConfiguration( __in IFileSystemConfiguration* const pfsconfig )
{
    m_pfsconfig_ = pfsconfig;
}

ERR CFlushMapForDump::ErrDumpFmPage( __in const FMPGNO fmpgno, __in const BOOL fDumpFlushStates )
{
    ERR err = JET_errSuccess;
    FlushMapPageDescriptor* pfmd = NULL;
    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );

    if ( ( fmpgno < 0 ) && !fFmHeaderPage )
    {
        Error( ErrERRCheck( JET_errInvalidParameter ) );
    }

    Alloc( pfmd = new FlushMapPageDescriptor( fmpgno ) );
    Call( ErrAllocateFmPage_( pfmd ) );
    pfmd->sxwl.AcquireWriteLatch();
    err = ErrReadFmPage_( pfmd, fTrue, 0 );
    pfmd->sxwl.ReleaseWriteLatch();
    
    if ( ( err < JET_errSuccess ) && ( err != JET_errReadVerifyFailure ) && ( err != JET_errPageNotInitialized ) )
    {
        Call( err );
    }

    err = JET_errSuccess;

    if ( fFmHeaderPage )
    {
        DumpFmHdr_( pfmd );
    }
    else
    {
        DumpFmDataPage_( pfmd, fDumpFlushStates );
    }

HandleError:

    FreeFmPage_( pfmd );
    delete pfmd;

    return err;
}

ERR CFlushMapForDump::ErrChecksumFmPage( __in const FMPGNO fmpgno )
{
    ERR err = JET_errSuccess;
    FlushMapPageDescriptor* pfmd = NULL;
    const BOOL fFmHeaderPage = FIsFmHeader_( fmpgno );

    if ( ( fmpgno < 0 ) && !fFmHeaderPage )
    {
        Error( ErrERRCheck( JET_errInvalidParameter ) );
    }

    Alloc( pfmd = new FlushMapPageDescriptor( fmpgno ) );
    Call( ErrAllocateFmPage_( pfmd ) );
    pfmd->sxwl.AcquireWriteLatch();
    err = ErrReadFmPage_( pfmd, fTrue, 0 );
    pfmd->sxwl.ReleaseWriteLatch();
    Call( err );

HandleError:

    FreeFmPage_( pfmd );
    delete pfmd;

    return err;
}
